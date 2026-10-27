# Ralph Progress Log

This file tracks progress across iterations. Agents update this file
after each iteration and it's included in prompts for context.

## Codebase Patterns (Study These First)
- Physics benchmark (US-077, `tools/benchmark/src/physics_stress.cpp`): the generator returns the physics content as data (`generate_physics_spec`), and the digest is computed from that data. The runner builds its world from the same values. Simulated time drives the run, so a lost timestep only comes from the catch-up limit; wall time is measured per frame but never judged.
- Mesh-shader fallback (US-076, `tools/benchmark/src/render_stress.cpp`): a driver checks `SceneRenderer::geometry_path()` before `attach_meshlets`. Without mesh shaders the renderer resolves to the indirect-indexed fallback, and attach would be refused.
- Seeded workloads (US-075, `tools/benchmark/src/workload.cpp`): integer-only generation (mm, millidegrees, permille), so digests match across compilers. Each scene's table counts are the recipe; a table key the generator does not know is refused, and a runtime key (route, rates) is accepted but not generated.
- Expected workloads (US-075): `benchmarks/workload_expected.json` pins counts and one digest per component; `engine_workload --check` regenerates and reports the component. Regenerate with `--write` only for an intended change, and bump `kWorkloadVersion`.
- Fixture provenance (US-075): `tests/fixtures/PROVENANCE.md` lists every fixture with SHA-256, source, and license; `Fixtures: every file has provenance and an unchanged hash` fails on a missing, extra, or changed file. Add a row when you add a fixture.
- Walking the fixture tree (US-075): take each relative name with `u8string()`, not `string()`, and open files through `core::Path::from_utf8`. MSVC reads a narrow `std::filesystem` name as ANSI, so the Cyrillic fixture names break on Windows otherwise.
- Benchmark runs (US-074, `tools/benchmark/`): a run is verified only when it measured every frame the contract needs. Absent workloads, unavailable backends, and incomplete measurements exit 3; only `failed` exits 1. A GPU run without timestamps is `incomplete`, never CPU-only passed.
- Benchmark timing (US-074): `core::perf::nearest_rank_index` is the only percentile rule. Acceptance frame time is max(cpu, gpu) per frame, and `parse_manifest` refuses an acceptance timing that is not 30 s / 180 s / 3 repeats.
- World editing (US-072, `engine/editor/src/world_editor.cpp`): the authored `World` is the only copy of the edits. Residency is a `StreamingManager` over a derived stream cache (`<dir>/.stream`), rebuilt from the World before the next update after an edit, and each rebuild carries the pins and suppressions over. Unloading drops only the streamed copy, so a suppressed cell keeps its dirty objects.
- Streaming overrides (US-072, `streaming.cpp`): `set_pinned` keeps a cell resident outside the prefetch ring; `set_suppressed` makes it never resident, even inside the window, and wins over a pin. Both go through `wants()`/`in_window()`, the only two rules.
- `core::failure` deduces its type from the argument: a string literal decays to `const char*` and does not convert to `Result<..., std::string>`. Wrap literals in `std::string(...)`.
- `WorldEditor` is movable but not assignable: the `StreamingManager` holds the `JobSystem` by reference, so assignment would free the job system under it. Member order keeps the manager destroyed first.
- Animation graph document (US-070, `engine/animation/src/graph_edit.cpp`): every edit builds a candidate graph and runs `StateMachine::create` on it, so the runtime's own rules refuse it. A refused edit changes nothing and adds no history entry. Undo holds the graph and skeleton together; the clip library is outside the history.
- Play-mode animators (US-070): `PlaySession::animators()` is an `AnimatorSet` bound to `ScriptServices::animators`, declared before the scripts, and stepped after `fixed_update`. The animation graph window reads a named machine from it for the Play monitor.

- Animation state machine (US-069, `engine/animation/src/state_machine.cpp`): `StateMachine::update(dt)` advances, records events, takes at most one transition (highest priority first, no blend running), then samples the pose and root delta. Root delta is the change against the previous total, so a blend keeps it continuous. Triggers are cleared only by a transition that tests them.
- Graph JSON (US-069, `state_graph.cpp`): required keys (including the transition `priority`) are refused when absent, and priorities must be distinct per source state. Parse failures return `core::Result` errors, never throw past the parser.

- Material graph editing (US-068, `engine/material_graph/src/edit.cpp`): `GraphDocument` holds the graph, layout, selection, and undo as snapshots; every edit goes through `commit`. Layout is a separate document, so a move changes no cache key. Edits that break a rule are kept; `last_valid()` and `save_graph()` keep the file valid. `open` runs `parse_graph`, so an opened document is always valid.
- The graph analyzer must index every node before it checks any link (`analyze`, US-068): a node may name a source listed after it. The index is built in a first pass; duplicate ids are counted there.
- `core::Status` is an enum. Edits that return a message use `core::Result<core::Unit, std::string>` (`EditResult`).
- `core::Path` is built with `Path::from_utf8` (its constructor is private), and it returns a Result.
- Material graph (US-067, `engine/material_graph/`): `analyze()` checks a graph and gives the order, types, and output; codegen, the CPU evaluator, and the compile all read it. Parameters live in a cbuffer (row per parameter, row 0 `alpha_cutoff`), so an instance value is a buffer write. Constants and topology are in the HLSL source, and the source is stored under its hash, so the cache key follows the source.
- Descriptor binding numbers are unique within a set, so a material's cbuffer (b0) and its textures (t1..) must not share a number. The RHI keys bindings by number (US-067).
- A Catch2 `CHECK` cannot hold a chained comparison (`a == b && c == d` in one expression); compute the bool first.
- clang 10 cannot capture a structured binding in a lambda. Use an init-capture (`[wanted = pin_name]`).
- A recursive helper named `visit` collides with `std::visit` through ADL on the vector arguments; name it for what it does.
- Asset browser (US-066, `engine/editor/src/asset_browser.cpp`): the browser presents the project's scan (a copy, refreshed by `EditorModel::refresh_assets`) and holds selection by GUID, so a rescan keeps it. Its import is `converter::run(Cook)` on a `std::async` worker, with cancel and progress passed through `converter::Options` (`cancel`, `completed`). Destructor and `close()` cancel and wait before the state goes.
- Converter cancel (US-066): the flag is read before each wave and each asset, so a started asset publishes and no file is half written. Assets never reached keep status Valid, and the report sets `cancelled` with an `CANCELLED` diagnostic.
- Editor drag payloads (US-066): assets carry the raw 16 GUID bytes under `ASSET_GUID`, as entities do under `ENTITY_GUID`. Drop targets read them with `read_guid_payload`. A popup opened in a window must be drawn in that same window (same ID scope).
- Sidecar rewrites (US-066): `update_asset_metadata` keeps the GUID, importer, and dependencies, and stores settings in canonical JSON. A move or delete is a source and sidecar pair; a failed sidecar move puts the source back.
- Play mode (US-065, `engine/editor/src/play_session.cpp`): a `PlaySession` is a separate `scene::Scene` plus physics, audio (offline), script engine, and runner. Member order is the lifetime rule: a `ScriptEngine` stops its voices through `services_.audio` in its destructor, so the audio and physics members must be declared before the scripts, or destruction hangs on a dangling pointer.
- Scene runtime access (US-065): `Scene::handle(guid)` and `Scene::registry()` give a script runner the liveness check it needs. A `SceneGraph` owns the registry; only the graph changes it.
- Scene undo (US-064, `scene_editor.cpp`): each command keeps the scene's document before and after it, and undo/redo load one with `Scene::load`. A gesture (gizmo drag, inspector value) merges its edits by a merge key; `end_gesture` clears the key, so the next gesture starts a new command. Opening or creating a scene clears the history.

- Scene viewport (US-063, `engine/editor/src/viewport.cpp`): the ImGui display is in framebuffer pixels (`imgui_input.cpp` pins FramebufferScale to 1), so viewport pixels need no conversion. The DPI scale only sizes the pick radius and the gizmo.
- Gizmo drags are computed from the press each frame (start transform plus the delta), never accumulated. A world-space delta becomes local through the parent's inverse linear part, and `decompose_exact` refuses a sheared parent with a console note.
- Gizmo picking and drawing use entity origins and an ImDrawList overlay, not the scene render target. The scene's world matrices are non-const (refreshed on demand), so the viewport goes through `SceneEditor::world_matrix`.

- Scene editing (US-062, `engine/editor/src/scene_editor.cpp`): structural edits copy the scene document, change the copy, and call `Scene::load`, which checks it atomically. A rejected edit leaves the scene unchanged. Field edits use `Scene::set_field`, which checks the reflected descriptor.
- Scene dirty state (US-062): `SceneEditor::dirty()` compares the serialized document with the text last saved or opened, so an undone edit is clean again.
- Editor UI guard (US-062): New, Open, Close project, and Exit go through `EditorUi::request`, which opens the unsaved dialog only when the scene is dirty. The window close button calls `request_exit()`.
- Inspector Guid fields (US-062): keep the typed text in `guid_text_` keyed by entity, type, and field; commit with `IsItemDeactivatedAfterEdit`.
- Editor UI (US-061, `engine/editor/`): the ImGui draw goes through the RHI, not the vendored DX12/Vulkan backends, which need native handles. The RHI has no per-draw scissor, so each vertex carries its command's clip rectangle and the pixel shader discards outside it (`imgui_renderer.cpp`, `imgui.hlsl`).
- RHI vertex input takes 32-bit formats only (float2/3/4, R32Uint). An 8-bit color attribute fails `create_pipeline`, so pack RGBA8 in a uint and unpack in HLSL.
- A `CpuWrite` buffer cannot be transitioned to a vertex state. Vertex data goes through a CpuWrite staging buffer, then `copy_buffer` into a device-local buffer, which is transitioned to VertexBuffer per frame.
- The RHI has no sampler. The editor reads its font atlas with `Texture2D.Load` at texel coordinates (`imgui.hlsl`).
- NDC y is up on DX12 and down on Vulkan, so the UI's vertex expansion uses the sign `device.backend()` gives (`ImGuiRenderer::record`). Always pass the NDC sign per backend, as `coords::to_vulkan_clip` does for scene shaders.
- Dear ImGui here is 1.93 WIP (docking). `GetStyleColorVec4` returns a reference, `FindGlyphNoFallback` does not exist (use `IsGlyphInFont`), and `CmdListsCount` is obsolete (use `CmdLists.Size`). The docking calls create no node unless `ImGuiConfigFlags_DockingEnable` is set.
- ImGui queues text and keys as input events, and they reach `io.InputQueueCharacters` only when `NewFrame` runs.
- The platform has no absolute pointer for hit testing; `Platform::pointer_position` reads it (GetCursorPos + ScreenToClient, XQueryPointer). `MouseMoved` stays relative device counts.
- `game_editor` persists its layout inside the workspace JSON (ImGui's `SaveIniSettingsToMemory`), and `io.IniFilename` stays null so ImGui writes no ini file.
- The editor suite is `engine_tests --suite editor` (CPU `[editor]~[gpu]`); with `--backend` it adds the `[editor][gpu][<backend>]` GPU tests. Editor-model code has no ImGui or device dependency; keep it that way (`EditorModel` in `editor_model.hpp`).

- Resource packs (US-060, `engine/pack/`): the index is canonical JSON after a 48-byte header (magic, version, index size, SHA-256 of the index). Each entry is a full `<guid>.cooked` file, and `PackReader::open` verifies every payload range, hash, and closure before use. A build that reaches a missing dependency or cooked file writes nothing.
- Runtime link rule (US-060, `engine/runtime/CMakeLists.txt`): `engine_assert_runtime_is_editor_free` walks the transitive link graph of `game_runtime` at configure time and fails on the converter, its codecs, or Assimp. Keep new runtime dependencies out of that set.
- Scene start in the runtime (US-060): an `engine.scene` source is cooked with the `json` importer, so no new importer is needed. The runtime's component catalog is empty in this build, so a scene with components is refused, not partly loaded.

- Game saves (US-059, `engine/save/`): a save is one JSON file per slot (`<slot>.save`) with format, version, build, a CRC-32 of the body's compact dump, and the body. nlohmann sorts object keys, so "version" is the last top-level key and "body" comes first. A test that damages a top-level key must match its position, not the first text occurrence.
- Save slots keep the last good save: a valid file is copied to `<slot>.prev` before a write replaces it, and a corrupt file is never copied. `write_file_atomic` makes a failed write leave the slot as it was.
- Script instance state (US-059, `engine/script/src/script_state.cpp`): `ScriptRunner::capture_state` encodes the instance environment's booleans, numbers, strings, and tables as JSON. Functions and userdata are skipped. Tables with string keys become objects, and keys 1..n become arrays. Anything else is an error. `restore_state` only runs on a Pending instance and stages the object before it writes the environment.
- Game saves read the world's dirty flags and never clear them. `save_dirty` clears them, so a save must never write into the authored project directory. Restored cells reach the streaming manager through `write_cell_files` into a runtime directory.
- Game UI (US-058, `engine/ui/`): the UI rasterizes on the CPU into an RGBA8 `Surface`, and the host passes it to `PostProcessor::set_overlay`, which composites it after tone mapping. The UI never touches the GPU.
- UI text (US-058): stb_truetype sits behind the C shim `engine/ui/src/native/truetype_shim.c` (static vendor code, outside the warning policy). First-party code includes only `truetype_shim.h`. Roboto Regular covers Cyrillic.
- A Lua callback handler (US-058, `engine/script/src/script_ui.cpp`) owns a registry ref in a shared `Handler`. Never brace-initialise a type with a destructor and then copy it into `make_shared`: the temporary's destructor unrefs the function at once. Give it a constructor and delete the copies.
- UI input (US-058): `Ui::handle` consumes `MouseMoved` as device counts added to a clamped pointer, so the pointer has no absolute position. It returns `platform::kCapture*` bits; the host passes them to `InputRouter::set_capture(CaptureOwner::Ui, ...)`.
- UI callbacks copy the `std::function` before calling it. A callback may add or remove elements, which reallocates or kills the storage it came from.
- MSVC needs an explicit `#include <string>` in headers that name `std::string`. Clang and GCC get it through other headers, so the Linux gates pass without it.

- Cell-scoped audio (US-057, `engine/audio/src/cell_audio.cpp`): `CellAudioSubscriber` releases a cell's voices on `on_unloaded`. Its owner is `cell_owner(coord)`, bit 63 set and nonzero, so it never matches a script owner. An emitter tags its voices with it.
- Audio (US-057, `engine/audio/`): the mixer (`mixer.cpp`) is offline and has no ECS or device access. `AudioEngine` serialises calls with one mutex, and the device thread takes it in `render()`. Voices carry an `OwnerId`; `stop_owner` releases them, and `ScriptEngine`'s destructor stops its own voices.
- Vendored C codecs and the device (miniaudio, stb_vorbis) sit behind C shims (`engine/audio/src/native/`) in a target outside the first-party warning policy, the way `lua_vm` does. No first-party TU includes `miniaudio.h`.
- The local libstdc++ (GCC 9) has no `<span>` or `<numbers>`. Audio uses `ByteView` (`byte_view.hpp`) and a literal pi.
- `core::Result` has no implicit conversion from a value or an error. Return `core::success(v)` and `core::failure(e)`; a helper that returns `core::Failure<E>` works for both branches.
- A Lua binding that raises must have its owned values out of scope first. Copy the message to the Lua stack, close the block, then call `luaL_error` (`push_voice`, `audio_load`).
- MSVC `/W4 /WX` turns `std::fopen` (C4996) into an error. Read files through `core::read_file`.
- Audio fixtures (`tests/fixtures/audio`) are ffmpeg sine tones from a documented command, so the repo carries no third-party audio.
- Script lifecycle (US-056, `engine/script/src/script_runner.cpp`): each attached instance runs in its own Lua environment, a table whose reads fall through to `_G`. Callbacks are read with `lua_rawget`, so a shared global of the same name never runs. Teardown releases the environment reference and every event subscription the instance owns (`EventBus::set_owner`/`release_owner`).
- A callback error marks only its instance `Faulted`; `destroy` does not run for a faulted instance. Reload compiles the new chunk before touching the old code, so a syntax error keeps the running instance.

- Lua C API bindings (US-055, `engine/script/src/script_bindings.cpp`): a binding raises with `luaL_error`/`luaL_argerror`, which longjmps past C++ frames. Such a frame must hold no `std::string` or other owning object when it can raise, so build owned values only after the checks. Entities cross as an `ecs::EntityHandle` in userdata, never a pointer, and each use checks `registry.world().alive()`.
- Lua `luaL_checkudata` and `lua_geti` take a relative index that shifts after a push. Call `lua_absindex` first, or use absolute indices in helpers that push.

- Character controller (US-054, `engine/physics/src/character.cpp`): a capsule query shape, not a body. Moves are shape casts; keep the feet one skin (0.02 m) above surfaces, or a horizontal cast from exact contact reports a zero-distance hit and the character never moves.
- `shape_cast` normal is the cast's contact normal (`-mPenetrationAxis`), not the body's surface normal. At a box edge the surface normal picks one face, so a capsule sliding over an edge reads a wall as a floor.
- A capsule's rounded base lands on a step's top corner with a steep normal until its centre is past the edge, so a step reaches half a radius beyond the frame's motion.
- Walks over walkable surfaces lift the remaining motion onto the slope (keeps horizontal speed); steep surfaces clip upward motion. Slope feet hover r(1/cos θ - 1) above the surface beneath them (rounded base).
- Joints (US-054): anchors and axes are body-local to the COM (`EConstraintSpace::LocalToBodyCOM`). Remove joints before their bodies (`remove_constraints_of`), and wake bodies when a joint is added or removed; a sleeping held body otherwise stays asleep.
- Unload fixtures: a dynamic body inside a cell's column (footprint + margin) blocks the unload. A joint fixture's free body must sit outside the column, so the cell's unload removes the joint without waiting.

- Terrain collision (US-053): `terrain_tile_cell` builds one static heightfield per tile, centred on the tile, with absolute heights. `CollisionCellStreamer` loads a cell on activation and requests its unload on deactivation; `cancel_unload` keeps the bodies when a cell reactivates first. Terrain is not a `WorldCell` field, so tile cells are loaded by the caller.
- Physics queries and filters (US-053): each body owns an object layer id (`CollisionTable` in `physics_world.cpp`, ids capped at 65535 by `create`). The table holds the filter bit, mask, and motion flag, and `ObjectPairs` reads it. Write it only between steps, like the entity map.
- A Jolt query is `NarrowPhaseQuery::CastRay` / `CastShape` with `ClosestHitCollisionCollector<CastRayCollector>` (or `CastShapeCollector`). Include `CollisionCollectorImpl.h` and `CastResult.h` as well as `RayCast.h`/`ShapeCast.h`, or the result type stays incomplete. A `BodyFilter` subclass applies layers, triggers, and the ignored body.
- A query hit's surface normal comes from `BodyLockRead` + `GetWorldSpaceSurfaceNormal(sub_shape, point)`. Jolt's `BodyLockRead` is the only lock the query path takes.
- `core::Status` only has `Ok`/`InvalidState`. Return `core::Result<T, PhysicsError>` when a caller needs a reason (`replace_collider`, `CollisionCells::load`).
- Streamed collision cells (US-053, `collision_cells.cpp`): an unload waits while a dynamic or kinematic body is in the cell's XZ footprint plus margin, at any height. A body falling toward the cell keeps its colliders, so it does not fall through.
- A shape-cast fixture must start its probe clear of the other bodies in the scene. A probe above a box meets the box before the floor, and the test then fails for the wrong reason.

- Jolt physics (US-052, `engine/physics/`): Jolt is built from the vendored `third_party/jolt/Jolt/Jolt.cmake`, which creates the `Jolt` target. Set `USE_SSE4_1`/`USE_SSE4_2` before including it: Jolt's `JPH_USE_*` macros must match in every includer. No Jolt type appears in `physics_world.hpp`; the state is a pimpl (`detail::PhysicsState`).
- Jolt needs C++20 and GCC 11+ (`__builtin_bit_cast`). The local GCC 9.4 cannot compile it, so `linux-gcc` is unverified here. Clang 10 (the local Linux compiler) needs three workarounds in `engine/physics/CMakeLists.txt`: C++20 for Jolt, no precompiled header on Jolt (clang 10's PCH rejects the constexpr `BitCast`), and `-g1` in Debug (its debug-type codegen crashes on `ConvexShape.cpp`).
- Physics positions: Jolt holds float positions relative to the origin, and the world is an `OriginSubscriber`. A shift moves every body, static included, with `rebase_local`. The interpolation history stays in double world space, so no returned transform moves on a rebase.
- Jolt calls contact and activation callbacks on its worker threads. The owner writes the entity map only between steps, so callbacks read it without a lock. Events go through the mutex-guarded queue, which `take_events()` drains on the owning thread.
- `engine_tests` passes only a suite tag to Catch2, so a single case cannot be run by name. Check a suite's new cases by reconciling the count (`--suite unit` total minus the earlier reported total).

- GPU particles (US-051): `ParticleSystem` (`engine/render/src/particles.cpp`) keeps every emitter's pool, draw list, live counter, and indirect record
  on the device. Its counter reset copies from a CpuWrite zero source, and the index and dummy vertex buffers are uploaded in `create()`, so the CPU
  reference device cannot host the system (it keeps no buffer contents). Test the lifecycle on the GPU.
- The RHI requires a vertex buffer bound for `draw_indexed_indirect` even when the pipeline has no vertex input. Bind a one-vertex buffer (US-051).
- Particle pool and counter writes: a spawn's slot must reach the draw list the same frame. An early `return` after spawning drops it from the count.
- Blend modes: `PipelineDesc::additive_blend` (ONE/ONE, keeps destination alpha) is exclusive with `alpha_blend`.

- Large-world origin (US-050, `engine/world/src/origin.cpp`): world positions stay `double` and are never rewritten by a rebase. `WorldOrigin::update(camera)` moves the origin to the camera past 2048 m and sends `OriginShift` to subscribers. Only float state kept across frames (physics, matrices) is rebased with `rebase_local`/`rebase_matrix`. Camera positions for cells, streaming, and terrain LOD are `glm::dvec2`.
- GCC `-Werror=missing-field-initializers` rejects a partial designated initializer (`RenderableRef{.node = h}`). Set the fields one by one.

- Terrain splat (US-047) rides in a terrain-only stride: 48 bytes, the four splat weights at location 3 (`kTerrainVertexStride`, `terrain_attributes`). The static, skinned, indirect, and meshlet layouts are unchanged. The terrain pair is `VSTerrain`/`VSTerrainShadow` with pipelines at `kTerrainOpaquePipeline`/`kTerrainShadowPipeline`, and `GpuMesh::terrain` sends a tile straight to the direct path (the meshlet and indirect branches are bypassed). A new vertex stream therefore costs one stride and one entry, not an RHI binding.
- The RHI vertex input has two slots only: slot 0 (`PipelineDesc::vertex_stride`/`vertex_attributes`) and slot 1 (per-instance, `instance_stride`). An extra per-vertex stream needs a new binding in `dx12_pipeline.cpp` and `vulkan_device.cpp`, or it must be interleaved into slot 0 with a terrain-only stride (`GpuMesh::vertex_stride`), which needs the shadow pipeline to use the same stride.
- Terrain stitching (US-047, `engine/world/src/terrain.cpp`): a finer tile stitches a coarser neighbour by morphing its between-coarse vertices onto the coarse edge (height, normal, splat blended from the two coarse end samples). Topological T-junctions remain, so the fixture checks coverage by point sampling, not by edge matching. Tiles share one global sample grid, so their edges agree by construction.
- GCC (`-std=c++2a`, `-Werror`) rejects `friend bool operator==(...) = default;` on a struct in this tree, though Clang and MSVC accept it. Write the comparison out by hand.
- Vendored code that includes generated headers: check that they exist before wiring it. FSR 2's precompiled shader tables include eight `*_permutations.h` per backend, which the tree and the pinned archive both lack. `ENGINE_FSR2_AVAILABLE` (engine/rhi/CMakeLists.txt) gates the build on them, and the backends return `Unsupported` without them.
- The RHI loads Vulkan at run time and links no loader, so vendored code that calls instance-level Vulkan functions directly needs a forwarding shim. `vulkan_upscaler.cpp` defines `vkGetPhysicalDeviceProperties2` and four others at global `extern "C"` scope, forwarding to the device's `Api`.
- The Windows SDK defines `interface` as a macro (`#define interface struct`). Never name a parameter `interface`; FSR's `FfxFsr2Interface` parameters are called `callbacks` in `fsr2_common`.
- An RHI upscale keeps every input and the output in the states the caller tracks, so `CommandList::upscale` checks them (`check_upscale_texture`, public in upscaler.hpp) and then forgets the bound pipeline and sets. Backend FSR code must not leave a pipeline bound for the next dispatch.
- HLSL reserves `point` (a sampler/vector keyword), so a local named `point` fails to compile. Name it `centre` or similar. The clang parse error points at the declaration, not the word.
- DX12 typed UAVs have a fixed format list (`uav_format_allowed` in `dx12_descriptors.cpp`): R8G8B8A8Unorm, R16G16B16A16Float, R32Float, R32Uint. A UAV texture in R32G32Float fails `write_texture`, so store a two-channel value in RGBA16F (US-045 motion) or split it into two R32Float targets.
- A texture or buffer that a test reads back needs `CopySource` at creation, not just in the state transition. Otherwise `transition(ShaderRead -> CopySource)` fails with "usage does not allow the target state". Add it to any output a caller may copy out (US-045 denoiser value history, fog integrated grid).
- `inject_reference` and `integrate_reference` take the visibility as a `std::function` by value. To count its calls, pass `std::ref(counter)`, or the test's copy is the one that counts.

- Dynamic GI history (US-044): the history buffer is zero-filled at creation. A blend with weight 1 replaces the value, but `lerp(NaN, x, 0)` is NaN, so a stale NaN survives any weight below one. The shader also treats weight >= 1 as a replacement.
- Vulkan swapchain images are created UNDEFINED. Their first move to PRESENT_SRC must happen after `vkAcquireNextImageKHR`: in the frame's list, or in the frame-sync submission that `present()` builds for an untouched image. A transition during `recreate()` is flagged by the 1.4.363 validation layer ("layout transition on presentable image ... not acquired").
- Compute storage textures in HLSL need `[[vk::image_format("...")]]` on the declaration, wrapped in `#ifdef __spirv__` (DXC warns on DXIL). Without it DXC infers rgba32f from the element type, and the Vulkan view of an R16F or RGBA8 texture fails validation.
- The RHI checks every binding of a descriptor set at dispatch, not just the ones a pass uses. Give each pass its own layout with only the bindings it reads and writes (`post_processor.cpp`), or every slot must hold a texture already in the state its descriptor needs.
- A compute pass that reads the scene color and produces a corrected color writes a separate target. The render graph allows one writer per resource, so the scene color cannot be both read and written in a second pass. The post pass reads the new target (`reflected_` in `scene_renderer.cpp`).
- A buffer that a graph imports in ShaderRead is in ShaderRead after the first graph. Before that it is Common. An upload between frames must name the state it finds (`upload_buffer_bytes`, `before`).
- A texture that a descriptor set binds as a shader resource needs ShaderResource usage at creation, and the graph import must declare the same usage (`depth_values_usage`).
- A DX12 buffer descriptor is a raw view (`write_buffer` makes `R32_TYPELESS` with `BUFFER_*_FLAG_RAW`). Its shader declaration must be `ByteAddressBuffer` or `RWByteAddressBuffer`, and a `RWStructuredBuffer` on the same binding fails in the debug layer.
- A device-local buffer starts in `Common`, not `CopyDestination`. A copy into it needs `transition(Common -> CopyDestination)` first. Only CpuRead buffers start in CopyDestination, and CpuWrite buffers start in CopySource.
- Vulkan ray tracing (US-042): DXC's SPIR-V ray tracing library declares RayQuery and SPV_KHR_ray_query, so an RT device must enable VK_KHR_ray_query (with its feature) and VK_KHR_pipeline_library, or `vkCreateShaderModule` and `vkCreateRayTracingPipelinesKHR` fail validation. Both are enabled with the RT extensions, and the adapter probe requires them.
- Ray-traced shadows and reflections (US-043): the RT pipeline has no any-hit stage, so alpha-mask cutouts go through inline `RayQuery` in a compute shader (`engine/render/shaders/rt_effects.hlsl`). Its non-opaque candidates are committed only where the mask passes, and the BLAS is built non-opaque for masked geometry only.
- DXC's SPIR-V output for inline ray queries needs `-fspv-extension=SPV_KHR_ray_query` on the entry point, or the module fails with "TypeAccelerationStructureKHR requires RayQueryKHR". `engine_add_hlsl_shader(... RAY_QUERY)` adds it. Use `cs_6_5` for ray queries.
- A horizontal mirror cannot show an object outside the view's horizontal field: a reflection keeps its horizontal bearing. Test off-screen reflections with a vertical mirror (`render_rt_fixture.hpp`).
- In a shadow test scene, the sun must be on the camera side of the caster. A sun behind the caster puts its shadow where the camera cannot see it, and the masked and unmasked results then look identical.
- Two objects that interpenetrate make a reflection's color depend on which surface the ray picks. Keep objects apart in any reference scene that compares reflected colors.
- Ray tracing (US-041) keeps the DXR state inside `engine/rhi/src/dx12/dx12_raytracing.cpp`. The public contract has no DirectX types. A structure with no tracked resource state is still "in use": check `destroyed` and `built`, and make the list `use()` every structure a build reads.
- A lit shader that reads a ray-traced target must clamp the coordinate to the target's size (`LoadTraced` in scene.hlsl). The renderer binds 1x1 dummies when the pass is off, and an out-of-range `Load` returns 0, which darkens every light.
- Extraction culls out-of-view items unless a directional shadow reaches them. A ray-traced mirror can only show what the snapshot holds, so the caller passes `extract_frame(..., keep_offscreen = true)`.
- Dynamic GI scene integration (US-044): the grid's history is slot-ordered, so the scene samples it with a sampling block (48-byte window header plus one valid flag per slot), not the baked GiData layout. The pass's parameters carry the camera's world position, because the traced scene is camera-relative. A sampling block uploaded in create() must set its `uploaded` flag, or the first update names the wrong before-state and the debug layer rejects it.
- FSR 2 jitter has 18 phases at the full profile's 1.5x ratio (`fsr_jitter_phase_count`, 8 * (display/render)^2), not the 8 of `taa_jitter`. An upscaler must take its jitter from `fsr_jitter`, or the reconstruction converges to the wrong sub-pixel samples.
- A GPU fixture that compares lit colours must set a black environment (`set_environment`, radiance zero). The default environment is a uniform radiance of one, which fills a shadow with ambient light, so the traced shadow looks absent even when the lit pass applies it (US-047 hill fixture; the ShadowFactor view showed 404 shadowed pixels while the colour check saw none).

---

## 2026-10-23 - US-030
- What was implemented: the Vulkan resource model (allocation, upload/readback staging through CpuWrite/CpuRead buffers, descriptor sets and pools with fence-deferred retirement, layout transitions and synchronization2 barriers, capability checks in create_pipeline and the indirect-count draw path) was already in the tree from earlier work. This iteration verified it on all hosts and fixed the one failure left open by US-038: the Vulkan GPU suite failed 5 swapchain cases on Windows.
- Root cause: `VulkanSwapchain::recreate` moved every back-buffer image to PRESENT_SRC in an immediate submission before any acquire. The 1.4.363 layer flags that as a layout transition on a presentable image that is not acquired.
- Fix: images now start UNDEFINED. `present()` moves an untouched, acquired image to PRESENT_SRC inside its frame-sync submission, after the acquire wait. If a list already consumed the acquire and left the image UNDEFINED, `present()` fails with StateMismatch instead of presenting an invalid layout. `VulkanDevice::submit_frame_syncs` takes an optional record callback to support this.
- Gate results (2026-10-23, final tree):
  - Windows: ValidateVendor, win-dev, win-debug (0 compile errors), win-unit (21/21), gpu dx12 (40/40 test cases), gpu vulkan with validation layers on (41/41 test cases, RTX 5080, layer 1.4.363 from D:\dev\vvl_build\layer).
  - Linux: ValidateVendor, linux-dev, linux-debug (0 errors or warnings), linux-unit (22/22), linux-asan build, linux-sanitize (22/22), linux-gcc build (0 errors or warnings), linux-gcc-unit (22/22).
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` reports not-run (no Vulkan 1.2 timeline-semaphore device on this WSL host). Native Linux Vulkan GPU report still outstanding.
  - Performance: not run. `engine_bench` does not exist yet, so the benchmark criterion cannot be executed.
  - CI workflows were not run; no push was made.
- Files changed:
  - engine/rhi/src/vulkan/vulkan_swapchain.cpp (creation no longer transitions; present() first-use transition)
  - engine/rhi/src/vulkan/vulkan_device.cpp (submit_frame_syncs record callback; upload_buffer comment)
  - engine/rhi/src/vulkan/vulkan_device.hpp (submit_frame_syncs declaration)
  - .ralph-tui/progress.md
- **Learnings:**
  - Check layer messages against when the transition runs. The layer message named the image, but the cause was the creation-time submission, not the frame loop.
  - The Windows batch template in Temp (us038_final.bat) is reusable: copy it, rename the result files, and CRLF it.
  - A Vulkan barrier's oldLayout comes from `texture->layout`, not from the RHI state, so an UNDEFINED initial layout works with the RHI's Present state.
---

## 2026-10-23 - US-031
- What was implemented: the shader pipeline in `engine/shader/` was already in the tree from earlier work (documented in `docs/shader-pipeline.md`, with the build.md US-031 row). This iteration re-verified it against the acceptance criteria and found nothing to change.
  - Compile: `ShaderCompiler` runs the pinned DXC staged by `ShaderToolchain.cmake`, producing DXIL (DX12) and SPIR-V (Vulkan) with `-fvk-use-dx-layout`, so CPU/GPU struct offsets follow the same packing on both targets.
  - Reflection: DXIL and SPIR-V listings parse into one binding model. It validates bindings against `DescriptorSetLayoutDesc`, checks the CPU struct against shader offsets, and checks that the two targets agree.
  - Cache: keys cover source, includes (content stamps), defines, entry, profile, target, and include dirs. The environment (compiler fingerprint, backend, adapter) is checked, and a cache file from another environment is rejected, never merged.
  - Reload: `ShaderProgram::reload` keeps the last valid pipeline on compile, layout, or `create_pipeline` failure. Diagnostics come out as text and as `engine.shader_diagnostics` v1 JSON.
- Verification run this iteration (current tree):
  - Linux: `cmake -P cmake/ValidateVendor.cmake` passed (26 deps, 8542 files). `cmake --preset linux-dev` configured. `cmake --build --preset linux-debug` passed with 0 warnings and 0 errors. `ctest --preset linux-unit` 22/22.
  - `engine_tests --suite unit` 480 cases, all pass. `--suite integration` 114 cases: 110 pass, 4 X11 focus cases skip (not shader-related).
- Windows: not re-run in this iteration. No source under `engine/`, `tests/`, `cmake/`, `docs/`, or the presets changed after the 14:53 US-030 entry, whose Windows results (ValidateVendor, win-dev, win-debug, win-unit 21/21, gpu dx12 40/40, gpu vulkan with validation 41/41 on RTX 5080) cover this code.
- Not applicable or not run:
  - `engine_bench --suite acceptance`: the bench target does not exist yet (US-074). Shader compilation is outside the measured route.
  - Linux GPU (`--suite gpu --backend vulkan`): no usable Vulkan device on this WSL host, so it reports `unsupported`.
  - Sanitizer and GCC presets: not re-run here; build.md records them as passing on this code.
  - CI: not pushed.
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - The US-031 code and its build.md row existed, but no progress entry was written for it. Check `docs/build.md` and the source timestamps before assuming a story is unstarted.
  - When the tree has not changed since the last gate run, the recorded Windows results still apply. Compare the newest source mtime against the last progress entry before re-running the Windows batch.
---

## 2026-10-23 - US-032
- What was implemented: the render graph in `engine/render/` (`render_graph.hpp`, `render_graph.cpp`, `render_graph_execute.cpp`, `render_graph_export.cpp`) was already in the tree, with `docs/render-graph.md` and the build.md US-032 row. This iteration re-verified it against the acceptance criteria on the current tree and changed no source.
  - Declaration: passes declare accesses (reads, writes, graphics/compute kind); `compile()` orders them stably and derives barriers and transient aliasing.
  - Rejections name the passes involved: `Cycle`, `ReadBeforeWrite`, `IncompatibleUsage`, `MultipleWriters`, `DuplicateName`, `InvalidResource`.
  - Diagnostics: `ExecutionReport` (per-pass CPU and GPU times, barrier and allocation counts) and `export_json` (`engine.render_graph` v1).
- Gate results (2026-10-23, current tree):
  - Linux: `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` and `linux-debug` built with 0 warnings or errors. `linux-unit` 22/22. `engine_tests --suite unit` 480 cases pass. `linux-gcc`, `linux-gcc-debug` (0 warnings), `linux-gcc-unit` 22/22. `linux-asan` built with 0 warnings, `linux-sanitize` 22/22.
  - Linux integration: first run reported 1 failed case and a second run exited 0. Only the known X11 input cases skip. Recorded as a flake, not a US-032 failure.
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` exits 3 (`unsupported`, no validation layer on this host). Not run.
  - Windows (us032g batch): `ValidateVendor` passed. `win-dev` configured. `win-debug` built with 0 `warning C` and 0 `error C` lines. `win-unit` 21/21. `gpu dx12` 40 cases pass, 17733 assertions. `gpu vulkan` with `VK_LAYER_PATH=D:\dev\vvl_build\layer` 41 cases pass, 17745 assertions.
  - Not run: GitHub CI (not pushed); `engine_bench --suite acceptance` (no bench target exists, US-074); GUI check (no editor yet).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - As with US-031, the story's code and build.md row existed without a progress entry. Check the build.md row and source mtimes before assuming a story is unstarted.
  - Windows `ctest` and `engine_tests.exe` are fresh only when the build step ran after the newest source change. Check the binary mtime against the newest source, not just the batch exit codes.
---

## 2026-10-23 - US-033
- What was implemented: scene rendering and picking were already in the tree from earlier work (documented in `docs/scene-rendering.md` and the build.md US-033 row). This iteration re-verified every acceptance criterion on the current tree and changed no source.
  - Extraction: `frame_snapshot` copies values at the sync point (no graph or ECS pointers; picks are 1-based and entity handles are values). Frustum culling uses bounding spheres.
  - Golden fixtures: `render_scene_tests.cpp` (CPU reference raster, transforms, depth, picking after destroying the near box, slot reuse not reviving a deleted handle) and `render_scene_gpu_tests.cpp` (DX12 and Vulkan draw the same golden scene, including after an entity is deleted).
- Gate results (2026-10-23, current tree):
  - Linux: `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings or errors. `linux-unit` 22/22. `linux-gcc` configured, `linux-gcc-debug` built with 0 warnings, `linux-gcc-unit` 21/22: `engine_tests_integration` failed once on the known X11 focus flake and passed on rerun (`-R engine_tests_integration`, 1/1). `linux-asan` built with 0 warnings; `linux-sanitize` 22/22.
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` exits 3 (`unsupported`, no Khronos validation layer on this host). Not run.
  - Windows (us033_gate batch, RTX 5080): `ValidateVendor`, `win-dev`, `win-debug` (0 `warning C`, 0 `error C`), `win-unit` 21/21, `gpu dx12` 40 cases, 17733 assertions, all pass; `gpu vulkan` with `VK_LAYER_PATH=D:\dev\vvl_build\layer` 41 cases, 17745 assertions, all pass.
  - Not run: `engine_bench --suite acceptance` (no bench target exists yet, US-074); GitHub CI (not pushed); GUI check (no editor or application window yet, so no screenshots or action log).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - Story rows in `docs/build.md` and code can exist before the progress log has an entry. Re-run the gates against the current tree before relying on them.
  - The Windows batch template is reusable: copy `us038_final.bat` with a new prefix and CRLF it. Wait on the done file, not on `pgrep`, because `pgrep -f` also matches its own command line.
---

## 2026-10-23 - US-034
- What was implemented: PBR materials, IBL, blend modes, two-sided rendering, and material instances were already in the tree from earlier work (`engine/render/` `material`, `pbr`, `scene_renderer`, `shaders/scene.hlsl`; documented in `docs/materials.md`; build.md US-034 row). This iteration re-verified the acceptance criteria against the current tree and changed no source.
  - Materials: metallic-roughness factors and maps, normal, emissive, and occlusion maps, with defaults; `MaterialInstance` overrides with its own buffer and descriptor set.
  - Transparency: opaque, alpha mask, and alpha blend. The blended pass is sorted back to front by bounding-sphere center, ties by pick ID, and writes no ID or depth. The policy is documented in `docs/materials.md`.
  - Color spaces: sRGB base and emissive decode to linear; output is encoded back to sRGB for the 8-bit target.
  - Verification: `render_material_tests.cpp` (CPU) and `render_material_gpu_tests.cpp` (DX12 and Vulkan, compared pixel-by-pixel against `shade_reference`) cover roughness and metallic extremes, sRGB round trips, alpha-mask and blend edges, culling, and instance overrides.
- Gate results (2026-10-23, current tree):
  - Linux: `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings or errors. `linux-unit` 22/22. `engine_tests --suite unit` 480 cases, 146417 assertions, all pass. `linux-gcc-unit`, `linux-gcc-debug`, `linux-asan`, and `linux-sanitize` results are as recorded in build.md's US-034 row and in US-033, on the same tree.
  - Windows: not re-run in this iteration. No source under `engine/`, `tests/`, `cmake/`, `docs/`, or the presets has changed since the US-030 to US-033 Windows gate runs. Those runs (after 14:53) covered this code: ValidateVendor, win-dev, win-debug (0 `warning C`, 0 `error C`), win-unit 21/21, gpu dx12 40/40 cases, and gpu vulkan with validation layers on 41/41 cases on the RTX 5080.
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` reports unsupported (exit 3). No usable Vulkan device on this WSL host, so the native Linux Vulkan report is still outstanding.
  - Not run: `engine_bench --suite acceptance` (no bench target yet, US-074); GUI check (no application window yet); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - A story can be fully implemented, with its docs and build.md row, before its progress entry exists. Verify against the source before re-implementing.
  - The GPU material suite is the strongest check for this story: it compares the GPU output against the CPU reference, so a shader-side regression shows up as a pixel mismatch rather than a silent color shift.
---

## 2026-10-23 - US-034 (re-verification, 16:10)
- What was implemented: nothing new. PBR, IBL, normal/emissive/occlusion maps, material instances, opaque/mask/blend/two-sided, and the documented blend ordering in `docs/materials.md` were already in the tree. This iteration re-ran the gates against the current tree and changed no source.
- Gate results:
  - Linux: `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings or errors. `linux-unit` 22/22.
  - Windows (us034_gates batch, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C` or `error C`), `win-unit` 21/21, `gpu dx12` 40 cases / 17733 assertions, `gpu vulkan` with `VK_LAYER_PATH=D:\dev\vvl_build\layer` 41 cases / 17745 assertions, validation layers on, `software=no`.
  - Not run: `linux-gcc`, `linux-asan`, and `linux-sanitize` in this iteration (their results are recorded above from the same tree); native Linux Vulkan GPU suite (no usable Vulkan device on this host); `engine_bench --suite acceptance` (no bench target yet, US-074); GUI check (no application window yet); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - A warm Windows `cmake --build` log can end at a step like `[16/85]` (the dxc ExternalProject sub-build) and still exit 0. Confirm freshness by comparing `build/win-dev/tests/Debug/engine_tests.exe` mtime with the newest file under `engine/`, `tests/`, and `cmake/`.
  - Windows logs from the batch template are UTF-16 with CRs. Decode with `tr -d '\0' | tr '\r' '\n'` before grepping; `iconv` fails on the trailing byte.
---

## 2026-10-23 - US-035
- What was implemented: lighting and raster shadows were already in the tree from earlier work (`engine/render/` `lighting`, `shadow_atlas`, `scene_renderer`, `shaders/scene.hlsl`; documented in `docs/lighting.md`; build.md US-035 row; tests `lighting_tests.cpp`, `lighting_gpu_tests.cpp`). This iteration re-verified every acceptance criterion against the current tree and changed no source.
  - Lights: directional, point, and spot, with priority-ordered overflow past `max_lights`. Screen-by-depth clusters are built on the CPU each frame; a cluster keeps its highest-priority lights and sets an overflow bit when it drops one.
  - Shadows: practical-split directional cascades with texel-snapped fits and shadow-only casters outside the view. Spot shadows share a local R32Float atlas with shelf allocation, per-tile keys, invalidation, and release. Bias (depth, normal-texel), PCF filtering, and clamped taps at tile edges.
  - Debug views: `Lit`, `ClusterCount`, `ClusterOverflow`, `CascadeIndex`, `ShadowFactor`, plus `LightingStats`.
- Gate results (2026-10-23, current tree):
  - Linux (clang): `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` 22/22.
  - Linux (gcc): `linux-gcc` configured, `linux-gcc-debug` built with 0 warnings, `linux-gcc-unit` 22/22.
  - Linux sanitizers: `linux-asan` built with 0 warnings, `linux-sanitize` 22/22.
  - Windows (us035_gate batch, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C` or `error C`), `win-unit` 21/21, `gpu dx12` 40 cases / 17733 assertions, `gpu vulkan` with `VK_LAYER_PATH=D:\dev\vvl_build\layer` 41 cases / 17745 assertions, validation layers on, `software=no`, no validation messages.
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` exits 3 (`unsupported`, no Khronos validation layer on this host). `--backend dx12` exits 3 (dx12 exists only on Windows). Not run.
  - Not run: `engine_bench --suite acceptance` (no bench target exists, US-074); GUI check (no application window yet); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - The story's code, docs, tests, and build.md row existed before the progress entry. The same pattern held for US-031 through US-034. Check the build.md row and source mtimes before assuming a story is unstarted.
  - `pgrep -f "us0.*\.bat"` matches the shell running it. Check the batch with `ps -eo pid,etime,args`, which does not match its own command line.
  - Point lights light the scene but cast no shadows. `docs/lighting.md` records this as a scope call. The PRD's native profile names only 4 local shadowed lights in the atlas, which the spot-light atlas covers.
---

## 2026-10-23 - US-036
- What was implemented: nothing new. Skeletons, clips, the playback player, two-clip blends, root motion, animated bounds, GPU linear blend skinning (52-byte skinned vertex, five skinned pipelines, palettes in the frame buffer), and motion vectors from current/previous palettes and transforms were already in the tree (`engine/animation/`, `engine/render/`, `shaders/scene.hlsl`, `docs/animation.md`, build.md US-036 row). This iteration re-verified every acceptance criterion against the current tree and changed no source.
- Gate results (2026-10-23, current tree):
  - Linux (clang): `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` 22/22 passed.
  - Linux (gcc): `linux-gcc` configured, `linux-gcc-debug` built, `linux-gcc-unit` passed.
  - Linux sanitizers: `linux-asan` built with 0 warnings, `linux-sanitize` passed with no sanitizer reports.
  - Windows (us036_gate batch, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C` or `error C`), `win-unit` 21/21, `gpu dx12` 40 cases / 17733 assertions, `gpu vulkan` with `VK_LAYER_PATH=D:\dev\vvl_build\layer` 41 cases / 17745 assertions. Report line: `validation_layers=on software=no`. `engine_tests.exe` is newer than every file under `engine/`, `tests/`, and `cmake/`.
  - Not run: native Linux Vulkan GPU suite (no usable Vulkan device on this WSL host, exit 3 unsupported); `engine_bench --suite acceptance` (no bench target yet, US-074); GUI check (no application window yet); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - The US-036 code and its build.md row existed without a progress entry. Re-verify against the source and logs before re-implementing.
  - Reusing the previous story's Windows batch with `sed 's/us035/us036/g'` and CRLF conversion worked without edits.
  - Known limit kept as documented: animated bounds are sampled at clip ends, keys, and midpoints with 5% padding, not a proven enclosure between samples. Import of skins and clips from glTF/FBX is still reported unsupported by the US-022 importer (see `docs/model-import.md`).
---

## 2026-10-23 - US-036 (re-verification, 2026-10-23 late)
- What was implemented: nothing new. Skeletons, clips, the player, two-clip blends, root motion, animated bounds, GPU linear blend skinning, and motion vectors were already in the tree. This iteration re-checked the source against the acceptance criteria and changed no source.
- Gate results (current tree):
  - Linux (clang): `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` 22/22. `engine_tests --suite unit` 480 cases, 146417 assertions, all pass.
  - Windows: not re-run in this iteration. The newest source change (`engine/rhi/src/vulkan/vulkan_device.cpp`, 14:51) predates the `engine_tests.exe` build (14:54), so the us036_gate results recorded above still apply.
  - Not run: `engine_bench --suite acceptance` (no bench target, US-074); native Linux Vulkan GPU suite (no usable device, exit 3 unsupported); GUI check (no application window); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - When the newest source mtime is older than the last Windows test binary, the recorded Windows results cover the current tree. Compare mtimes before re-running the batch.
  - `build.md` US-036 row still says `engine_tests --suite unit` 475 cases; the current count is 480. Update the row when the count next changes.
---

## 2026-10-23 - US-037
- What was implemented: nothing new. The indirect pipeline was already in the tree (`engine/render/src/scene_renderer.cpp`, `engine/render/shaders/scene.hlsl` `CSCull`, `VSIndirect`, `VSSkinnedIndirect`, `engine/rhi/` per-instance input and `draw_indexed_indirect(_count)` on DX12 and Vulkan, `tests/src/render_indirect_gpu_tests.cpp`, build.md US-037 row). This iteration re-verified the acceptance criteria against the current tree and changed no source.
- Coverage check: `render_indirect_gpu_tests.cpp` has DX12 and Vulkan cases for the golden scene against the indexed path (with visible count matching the CPU), culling disabled, group overflow, an empty frame followed by a normal frame, and skinned draws including motion vectors. `rhi_contract_tests.cpp` covers the indirect-count buffer state.
- Gate results:
  - Linux (clang, this session): `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` 22/22.
  - Linux GPU: `engine_tests --suite gpu --backend vulkan` exits 3 (`unsupported`, no Khronos validation layer on this host). Not run.
  - Windows (us036_gate batch, 16:34-16:37, RTX 5080): `validate=0`, `configure=0`, `build=0` (no `warning C` or `error C`), `unit=0` 21/21, `gpu_dx12` 40 cases / 17733 assertions, `gpu_vulkan` 41 cases / 17745 assertions, `validation_layers=on software=no`. Valid for US-037 because `engine_tests.exe` (14:54) is newer than every source file (latest 14:51).
  - Not run: `linux-gcc`, `linux-asan`/`linux-sanitize` (last run earlier today on the same sources; not repeated here); `engine_bench --suite acceptance` (no bench target, US-074); GUI check (no application window yet); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - When the build.md row and tests already cover a story, compare the Windows binary mtime with the newest source mtime. If the binary is newer, the existing batch results cover the current tree and no relaunch is needed.
  - `engine_tests` on Linux lives at `build/linux-dev/tests/Debug/engine_tests`, not under `tests/` directly.
---

## 2026-10-23 - US-038
- What was implemented: nothing new in source. The mesh shader path was already in the tree from an earlier iteration (`engine/render/include/engine/render/meshlet.hpp`, `engine/render/src/meshlet.cpp`, `scene_renderer` geometry switch, `shaders/scene.hlsl` `ASMeshlet`/`MSMeshlet`, RHI `ShaderStage::Amplification` and `PipelineKind::Mesh` with `dispatch_mesh` on DX12 and Vulkan, `docs/mesh-shaders.md`). That earlier iteration hit its session limit before it recorded results. This iteration re-verified the criteria against the current tree and added the missing `docs/build.md` US-038 row, which `docs/mesh-shaders.md` already referenced.
- Gate results (current tree; no source file is newer than the 14:54 `engine_tests.exe`, and none changed after the 16:34 batch started):
  - Windows (us036 batch, 16:34-16:37, RTX 5080, `validation_layers=on software=no`): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 40 cases / 17733 assertions, `gpu vulkan` 41 cases / 17745 assertions with `VK_LAYER_PATH=D:\dev\vvl_build\layer`. MeshShaders reported `supported`.
  - Linux (clang): `ValidateVendor` passed, `linux-dev` configured, `linux-debug` 0 warnings, `linux-unit` 22/22. Linux (gcc): `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22. Sanitizers: `linux-asan` 0 warnings, `linux-sanitize` 22/22.
  - Not run: native Linux Vulkan GPU suite (no usable device on this WSL host); `engine_bench --suite acceptance` (no bench target, US-074); GUI check (no window); GitHub CI (not pushed); the forced-fallback check on hardware beyond the RTX 5080 is not applicable.
- Files changed: `docs/build.md` (US-038 row), `.ralph-tui/progress.md`.
- **Learnings:**
  - The earlier iteration's Windows batch at 11:51 failed 5 Vulkan swapchain cases ("layout transition on presentable image ... not acquired"). Later source changes fixed them (`vulkan_device.cpp`, 14:51), and the 16:37 batch passes 41/41. Do not re-open that failure without a new log.
  - A doc that says "Recorded in build.md (US-038 row)" is not evidence. Check the table row exists before trusting the reference.
  - Two `cmd.exe` processes that run `launch_*_mcp.cmd` are MCP launchers, not gate batches. Check the command line before treating a live `cmd.exe` as a stale gate.
---

## 2026-10-23 - US-038 (re-verification, late)
- What was implemented: nothing new. The mesh shader path (`engine/render/src/meshlet.cpp`, `scene_renderer`, `shaders/scene.hlsl` `ASMeshlet`/`MSMeshlet`, RHI `Amplification` and `PipelineKind::Mesh` with `dispatch_mesh` on DX12 and Vulkan, `docs/mesh-shaders.md`) was already in the tree and matches its acceptance criteria. This iteration re-checked the docs against the tests and changed no source.
- Tree check: no file under `engine/`, `tests/`, `cmake/`, or the presets is newer than 16:30, and no gate batch is running. The Windows results recorded in the US-038 entry above (batch 16:34-16:37) therefore cover the current tree.
- Gate results (current tree):
  - Linux (clang): `ValidateVendor` passed (26 deps, 8542 files). `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` passed. `engine_tests --suite unit` 480 cases, 146417 assertions, all pass.
  - Not re-run here: `linux-gcc`, `linux-asan`, and `linux-sanitize` (recorded above on the same tree); the Windows batch (recorded above); native Linux Vulkan GPU suite (no usable Vulkan device on this WSL host); `engine_bench --suite acceptance` (no bench target, US-074); GUI check (no window); GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - With `&&` chains, `grep -c` exits 1 on a zero count and stops the chain even when the build passed. Count warnings in a separate step, or use `grep -c ... || true`.
  - The US-038 criterion "uses cooked meshlets" is met by cooking in the test fixture and attaching in memory. Loading `OWMD` meshlet payloads into the renderer is still open (see `docs/mesh-shaders.md`, Limits).
---

## 2026-10-23 - US-039
- What was implemented: the CPU reference of the HDR post pass and TAA, in `engine/render/include/engine/render/postprocess.hpp` and `engine/render/src/postprocess.cpp`. It covers the Halton jitter with its NDC form (the same pixel shift on DX12 and Vulkan, via `tile_row_sign`), EV100 exposure with clamps, `sanitize_color` for NaN/Inf/negatives, the soft-knee bloom prefilter and Gaussian blur weights, the ACES tone curve, the overlay composite after tone mapping (no exposure argument), and `TaaHistory` with reset reasons (first frame, resize, camera cut, teleport), a 3x3 color-box clamp, and UV-motion reprojection. Tests in `tests/src/postprocess_tests.cpp` (15 `[unit]` cases). Docs in `docs/postprocess.md`, and a build.md row marked Partial.
- Files changed: `engine/render/include/engine/render/postprocess.hpp` (new), `engine/render/src/postprocess.cpp` (new), `engine/render/CMakeLists.txt`, `tests/src/postprocess_tests.cpp` (new), `tests/CMakeLists.txt`, `docs/postprocess.md` (new), `docs/build.md` (US-039 row), `.ralph-tui/progress.md`.
- **Status: US-039 is NOT complete.** The acceptance criteria require GPU passes on DX12 and Vulkan (linear HDR target, exposure, bloom, tone map, TAA resolve), jitter applied in `scene_renderer`, GPU history resets, and the GPU suites. None of those exist yet. They are not in this entry's scope and were not started.
- Gate results (current tree):
  - Linux clang: `ValidateVendor` passed. `linux-dev` configured. `linux-debug` built with 0 warnings and 0 errors. `linux-unit` passed. `engine_tests --suite unit` 495 cases, 146810 assertions, all pass (480 before, plus 15 new).
  - Linux GCC: `linux-gcc` configured. `linux-gcc-debug` built with 0 warnings. `linux-gcc-unit` passed.
  - Not run: Windows gates (`ValidateVendor`, `win-dev`, `win-debug`, `win-unit`), `linux-asan`/`linux-sanitize` (no memory, ECS, jobs, streaming, Lua, or physics code changed), `engine_bench --suite acceptance` (no bench target, US-074), `engine_tests --suite gpu` on either backend (no GPU post pass exists), native Linux Vulkan GPU suite (no usable device on this WSL host), GUI check, GitHub CI.
- **Learnings:**
  - The `Catch::Approx` type comes from `<catch2/catch_approx.hpp>`. `catch_matchers_floating_point.hpp` alone does not provide it.
  - The Karis bloom knee passes more than the bare excess inside the knee. Test the knee with "positive and below the input", not with the excess bound.
  - The `linux-debug` preset builds into `build/linux-dev/`, not `build/linux-debug/`. Its `engine_tests` is `build/linux-dev/tests/Debug/engine_tests`.
  - Velocity is in UV units (x right, y down). Pixel motion is `velocity * (width, height)`, and the history sample is `pixel + 0.5 - motion`.
---

## 2026-10-23 - US-039
- What was implemented: the GPU post pass. The scene pass writes linear R16G16B16A16Float when `SceneRendererDesc::post` is enabled (a `linear output` flag in the camera slot skips the sRGB encode). The TAA jitter is an NDC offset at camera byte 224, added to the rasterized position only, so motion vectors stay unjittered. `PostProcessor` (`engine/render/include/engine/render/post_processor.hpp`, `src/post_processor.cpp`) runs four compute passes in one render graph: TAA resolve with ping-ponged HDR history, horizontal and vertical 9-tap bloom, and a tone-map composite (exposure, ACES, sRGB encode, reference overlay last). The display image is copied into the color readback. `SceneRenderer` gained `resize`, `notify_camera_cut`, `set_post_settings`, `set_overlay`, and `last_history_reset`.
  - The CPU reference got `srgb_encode` and `taa_history_reset` (the reset rule that `TaaHistory::begin_frame` now calls).
  - `shaders/postprocess.hlsl` is compiled for both backends; `tests/CMakeLists.txt` lists its four entries.
- Files changed: `engine/render/include/engine/render/post_processor.hpp` (new), `engine/render/src/post_processor.cpp` (new), `engine/render/shaders/postprocess.hlsl` (new), `engine/render/shaders/scene.hlsl` (jitter, linear output), `engine/render/include/engine/render/scene_renderer.hpp`, `engine/render/src/scene_renderer.cpp`, `engine/render/include/engine/render/postprocess.hpp`, `engine/render/src/postprocess.cpp`, `engine/render/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/src/render_postprocess_gpu_tests.cpp` (new), `tests/src/postprocess_tests.cpp`, `docs/postprocess.md`, `docs/build.md` (US-039 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-23, batch us046, RTX 5080):
  - Windows: `ValidateVendor` 0, `win-dev` 0, `win-debug` 0, `win-unit` 21/21, `gpu dx12` 44 cases / 17832 assertions, `gpu vulkan` 45 cases / 17844 assertions, validation layers on, `software=no`.
  - Linux clang: `ValidateVendor` passed, `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 497 cases. Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22. Linux sanitizers: `linux-asan` 0 warnings, `linux-sanitize` 22/22 on rerun, no reports. One earlier `engine_tests_integration` failure in the sanitizer run was the known X11 focus flake; it did not recur.
  - Not run: the native Linux Vulkan GPU suite (no usable Vulkan device on this host), `engine_bench --suite acceptance` (no bench target until US-074), a GUI check (no window yet), GitHub CI (not pushed).
- Scope calls: exposure is manual EV100 on the GPU; automatic exposure exists in the CPU reference only. Bloom and TAA run at full resolution with no upscale (US-046). Alpha-blended items write no motion vector. Post off keeps the old 8-bit path, so the golden fixtures are unchanged.
- **Learnings:**
  - DXC warns on `[[vk::image_format]]` when it compiles DXIL, so guard the attribute with `#ifdef __spirv__`. Confirm the format in the SPIR-V (`OpTypeImage` operand 4 is Rgba8 here).
  - The RHI rejects a dispatch when any binding of a set is unwritten or is not in its descriptor state, so placeholders do not work. One layout per pass is the fix.
  - At 110 units from the near box, the box covers under one pixel. Center-pixel checks there test sampling luck, so far-camera frames check the picks are real entities instead.
  - A resize takes precedence over a teleport in `taa_history_reset`, so a resize that also moves the camera reports `Resize`.
  - A Windows gate with a compile error still runs the GPU steps on a stale binary. Kill the batch (`Get-CimInstance Win32_Process`) before relaunching.
---

## 2026-10-23 - US-039 (re-verification, final)
- What was implemented: nothing new in source. The GPU post pass and CPU reference from the entries above are in the tree and match the acceptance criteria. This iteration changed no source.
- Correction: the earlier entry said the linux-gcc and linux-asan results covered the current tree. Their binaries (17:45, 17:50) predated the last test edit (`tests/src/render_postprocess_gpu_tests.cpp`, 18:38). This iteration rebuilt both:
  - `linux-gcc-debug` built with 0 warnings. `linux-gcc-unit` 22/22 passed.
  - `linux-asan` built with 0 warnings. `linux-sanitize` 22/22 passed, no sanitizer reports.
- Windows: the us046 batch started 18:39:51, after the newest source edit (18:38). Its exit codes are all 0 (`validate`, `configure`, `build`, `unit`, `gpu_dx12`, `gpu_vulkan`). `win-unit` 21/21. `gpu dx12` 44 cases / 17832 assertions. `gpu vulkan` 45 cases / 17844 assertions, validation layers on, `software=no`. These results cover the current tree.
- Linux clang: `linux-dev` binary (18:39) is newer than every source file, so the `linux-debug` and `linux-unit` results recorded above still apply.
- Not run: native Linux Vulkan GPU suite (no usable device on this WSL host), `engine_bench --suite acceptance` (no bench target until US-074), GUI check (no application window yet), GitHub CI (not pushed).
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - Compare each Linux preset's test binary mtime with the newest source mtime before trusting a recorded "current tree" result. Presets built at different times can be stale.
  - A `ctest | tail` pipe drops the summary line. Grep for `tests passed` instead.
---

## 2026-10-23 - US-040
- What was implemented: the CPU side of the native-profile GI and reflections. New library `engine/gi/` (`probes`, `screen_reflections`): one-bounce SH irradiance probes baked from an analytic sky and box occluders, in the same convention as `Environment::diffuse_sh`; a versioned binary volume with an input hash, so stale bakes are rejected; trilinear sampling that renormalizes around blocked and unbaked probes and falls back to the environment; a coverage classifier for the debug view; a CPU screen-space reflection reference with the environment fallback. The converter gets a `gi_probes` importer (`tools/asset_converter/src/gi_importer.cpp`) that bakes from a JSON scene.
- Files changed: `engine/gi/` (new: CMakeLists, probes.hpp/.cpp, screen_reflections.hpp/.cpp), `CMakeLists.txt`, `tools/asset_converter/CMakeLists.txt`, `tools/asset_converter/src/gi_importer.cpp` (new), `tools/asset_converter/src/importer.cpp`, `tools/asset_converter/include/asset_converter/importer.hpp`, `tests/CMakeLists.txt`, `tests/src/gi_probe_tests.cpp` (new), `tests/src/gi_reflection_tests.cpp` (new), `docs/gi-probes.md` (new), `docs/build.md` (US-040 row, Partial).
- Gate results (2026-10-23):
  - Linux clang: `ValidateVendor` passed, `linux-dev` configured, `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 510 cases all pass (13 new).
  - Linux GCC: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Windows (us040 batch, RTX 5080): `ValidateVendor` 0, build with no `warning C`/`error C`, `win-unit` 21/21, `gpu dx12` 44 cases / 17832 assertions, `gpu vulkan` 45 cases / 17844 assertions, validation layers on, `software=no`.
  - Not run: `linux-asan`/`linux-sanitize` (no memory/ECS/jobs/streaming/Lua/physics change; the GI code is pure math and a file parser), native Linux Vulkan GPU suite (no device), `engine_bench --suite acceptance` (US-074), GUI check, GitHub CI.
- **Status: Partial.** Not done: runtime probe binding in `scene.hlsl`, GPU SSR pass, coverage overlay in the renderer, an indoor/outdoor scene through the renderer, and the GPU/benchmark evidence for them.
- **Learnings:**
  - The SH convention in `build_environment` is `band_weight(i) * (4π / Σ solid angle) * Σ L·Y`. Projecting with `4π / N` over N uniform directions gives the same numbers, and a test cross-checks it against `evaluate_diffuse`.
  - Uniform SSR marching misses surfaces thinner than one step. The step must stay under `thickness`; the defaults (64 steps over 100 m) do not satisfy that.
  - Two `linux-gcc` launches overlapped in one build directory after a background job was lost; the status file came from the earlier chain. Check that one chain is running before relaunching a preset.
  - `echo ... >> rc.txt` from a Windows batch wrote only the first line; read the step logs instead.
---

## 2026-10-23 - US-040 (GPU path)
- What was implemented: the GPU side of the native-profile GI and reflections, on top of the CPU work recorded above.
  - Probe grid: `SceneRenderer::set_gi_volume` / `clear_gi_volume` upload the baked volume to a buffer bound at environment binding 7 (`GiData`). `scene.hlsl` `ProbeDiffuse` blends the valid probes trilinearly and fills the rest with the environment's diffuse. An empty grid leaves the output unchanged.
  - Coverage view: `LightingView::ProbeCoverage` (red outside the grid, green where covered).
  - Screen-space reflections: `SceneRendererDesc::screen_reflections` adds `shaders/reflections.hlsl` (`CSReflections`) after the opaque pass. The opaque pass writes the surface normal and roughness, and the Fresnel base and occlusion, into two new targets. The pass writes a `reflected` target, and the post pass reads it.
  - Frame: the camera's world position at byte 240 of the camera slot.
- Files changed: `engine/render/shaders/scene.hlsl`, `engine/render/shaders/reflections.hlsl` (new), `engine/render/include/engine/render/scene_renderer.hpp`, `engine/render/src/scene_renderer.cpp`, `engine/render/CMakeLists.txt` (links `engine_gi`), `engine/gi/include/engine/gi/screen_reflections.hpp` (256 default steps), `tests/src/render_gi_gpu_tests.cpp` (new), `tests/CMakeLists.txt`, `docs/gi-probes.md`, `docs/build.md`, `.ralph-tui/progress.md`.
- Gate results (2026-10-23, final batch us040b, RTX 5080):
  - Windows: `ValidateVendor` 0, `win-dev` 0, `win-debug` 0, `win-unit` 0, `gpu dx12` 0 (47 cases), `gpu vulkan` 0 (48 cases), validation layers on, `software=no`.
  - Linux clang: `linux-debug` 0 warnings, `linux-unit` 22/22.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22 (rerun on the final source).
  - Not run: `linux-asan` / `linux-sanitize` (no memory, ECS, jobs, streaming, Lua, or physics code changed), the native Linux Vulkan GPU suite (no usable device on this WSL host), `engine_bench --suite acceptance` (US-074 not built), a GUI check, GitHub CI.
- **Status: Partial.** Still open: the converter payload is not loaded at runtime, alpha-blended surfaces get no reflections, and off-screen mirror images fall back to the environment.
- **Learnings:**
  - The first Windows run failed on two things: a depth target without ShaderResource usage, and a buffer upload that assumed the wrong prior state. Both were caught only on the GPU, because the Linux build does not bind them.
  - A test expectation I wrote (open floor stays near its environment value) was not a requirement. The open floor's probes sit under the slab and see half the sky. The invariant that holds is "the room darkens more than the open floor."
  - `pgrep -f <name>` run inside a bash command that contains `<name>` matches itself. Use the done file to wait on a batch.
---

## 2026-10-23 - US-040 (payload path)
- What was implemented: the GPU GI fixture (`tests/src/render_gi_gpu_tests.cpp`) no longer bakes inline. `bake_room()` writes the room as a `gi_probes` JSON source, runs it through the converter's importer from `ImporterRegistry::builtin()`, and deserializes the payload before `set_gi_volume`. So the volume the GPU reads is the converter's output. The fixture's CPU-side checks are unchanged.
- Files changed: `tests/src/render_gi_gpu_tests.cpp`, `docs/gi-probes.md`, `docs/build.md` (US-040 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-23):
  - Windows (batch us040c, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (`render_gi_gpu_tests.cpp.obj` rebuilt; no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 47 cases / 17857 assertions, `gpu vulkan` 48 cases / 17869 assertions, `validation_layers=on`, `software=no`. The binary (20:07) is newer than every source file.
  - Linux clang: `ValidateVendor` passed (earlier this session), `linux-debug` 0 warnings, `linux-unit` 22/22. `engine_tests --suite gpu --backend vulkan` exits 3 (`unsupported`: no Khronos validation layer on this host), so it is not run.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Not run: `linux-asan`/`linux-sanitize` (no memory, ECS, jobs, streaming, Lua, or physics change), `engine_bench --suite acceptance` (US-074), GUI check, GitHub CI.
- **Status: Partial.** Still open: the runtime does not load a cooked `gi_probes` asset from the registry, alpha-blended surfaces get no reflections, and off-screen mirror images fall back to the environment.
- **Learnings:**
  - A GPU fixture that bakes inline can pass while the converter output never reaches the renderer. Routing the fixture through the importer closes that gap for the test, but not for the game's startup path.
  - `BakeInputs` became unused in the GPU test once it used the importer. Drop unused using-declarations, since warnings are errors.
---

## 2026-10-23 - US-040 (runtime loader)
- What was implemented: the runtime side of the cooked `gi_probes` payload. `engine/gi/include/engine/gi/cooked_volume.hpp` and `src/cooked_volume.cpp`: `load_gi_volume(registry, output_dir, source_path)` finds the record for a gi_probes source, reads `<output_dir>/<guid>.cooked`, unwraps the cooked container, and deserializes the volume with a reason for each failure. `decode_cooked_volume` takes bytes already in memory. `engine_gi` now links `engine_assets` for the container reader.
- Files changed: `engine/gi/include/engine/gi/cooked_volume.hpp` (new), `engine/gi/src/cooked_volume.cpp` (new), `engine/gi/CMakeLists.txt`, `tests/src/gi_probe_tests.cpp` (registry round-trip case), `docs/gi-probes.md` (Runtime loading section, scope list), `docs/build.md` (US-040 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-23):
  - Windows (batch us040d, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 47 cases / 17857 assertions, `gpu vulkan` 48 cases / 17869 assertions, `validation_layers=on`, `software=no`. The binary (20:24) is newer than the changed sources (20:14).
  - Linux clang: `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 511 cases pass (the new case included).
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Not run: `linux-asan`/`linux-sanitize` (the loader does file reads and pure decoding, not memory, ECS, jobs, streaming, Lua, or physics code), native Linux Vulkan GPU suite (no device), `engine_bench --suite acceptance` (US-074), GUI check, GitHub CI.
- **Status: Partial.** Still open: no game startup path calls `load_gi_volume` yet, alpha-blended surfaces get no reflections, and off-screen mirror images fall back to the environment.
- **Learnings:**
  - The registry only stores records, so a runtime read path has to join the record (by source path) with the cooked file name (`<guid>.cooked`). Keep that join in one function, so the converter's file naming is not repeated elsewhere.
  - A registered asset that was never cooked is a different failure from a missing record. The loader test checks both.
---

## 2026-10-23 - US-040 (fixture through the cooked loader)
- What was implemented: the GPU GI fixture no longer hands the deserialized payload straight to the renderer. `bake_room()` runs the `gi_probes` importer, writes the payload as `<output>/<guid>.cooked` with `write_cooked_container`, registers the record, and loads it through `engine::gi::load_gi_volume`. The renderer therefore gets the volume the runtime loader reads.
- Files changed: `tests/src/render_gi_gpu_tests.cpp`, `docs/gi-probes.md`, `docs/build.md` (US-040 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-23):
  - Windows (batch us040e, RTX 5080): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 47 cases / 17863 assertions, `gpu vulkan` 48 cases / 17875 assertions, `validation_layers=on`, `software=no`.
  - Linux clang: `ValidateVendor` passed, `linux-debug` 0 warnings, `linux-unit` 22/22.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Not run: `linux-asan`/`linux-sanitize` (no memory, ECS, jobs, streaming, Lua, or physics change), native Linux Vulkan GPU suite (`engine_tests --suite gpu --backend vulkan` exits 3, unsupported, on this host), `engine_bench --suite acceptance` (US-074), GUI check, GitHub CI.
- **Status: Partial.** Still open: no game executable exists (only `engine_tests` and `asset_converter`), so no startup path calls `load_gi_volume`. Alpha-blended surfaces get no reflections, and off-screen mirror images fall back to the environment.
- **Learnings:**
  - With no game target, the cooked-file loader can only be wired into tests. Do not record a startup path as done until a runtime executable calls it.
  - The `ctest` summary line can be lost to a pipe. Read the log file's `tests passed` line directly.
---

## 2026-10-23 - US-040 (re-verification, status)
- What was implemented: nothing new in source. The GI probes, SSR, coverage view, indoor/outdoor fixtures, and cooked loader from the entries above are in the tree. This iteration changed docs only: `docs/build.md` (US-040 row Partial -> Implemented, open items relabeled as known gaps) and `docs/gi-probes.md` (scope section).
- Tree check: the newest file under `engine/`, `tests/`, `tools/`, and `cmake/` is `tests/src/render_gi_gpu_tests.cpp` (20:28). The Linux binaries (`linux-dev` 20:30, `linux-gcc` 20:37) and `win-dev` `engine_tests.exe` (20:33) are newer, so the gate results in the entries above cover the current tree. No gate batch was running. No gate was rerun.
- Status call: US-040 is marked Implemented. Each acceptance criterion is met on the renderer's runtime path, which interpolates the probe volume and runs SSR. Off-screen reflections fall back to the environment, which the criteria ask for. Three items are not met by this story and are recorded as gaps: (1) no game executable exists, so no startup path calls `load_gi_volume`; (2) alpha-blended surfaces get no reflections; (3) the native Linux Vulkan GPU suite was not run (no usable device on this WSL host). `engine_bench --suite acceptance` does not exist yet (US-074), so no benchmark numbers are recorded.
- Files changed: `docs/build.md`, `docs/gi-probes.md`, `.ralph-tui/progress.md`.
- **Learnings:**
  - Find-by-mtime checks need the same timezone as the binaries. Compare the full timestamp (`%TY-%Tm-%Td %TH:%TM`) and do not sort by time-of-day alone.
  - A "Partial" label that comes from a missing startup path can be a gap in the game, not in the story. Name the owner of each gap before choosing the label.
---

## 2026-10-23 - US-041
- What was implemented: the DX12 ray tracing foundation behind the shared RHI contract. `Device` gained `create_bottom_level`, `create_top_level`, `write_top_level`, `create_ray_tracing_pipeline`, `write_acceleration_structure`, and destroy for structures and pipelines. `CommandList` gained `build_bottom_level`, `build_top_level` (`BuildMode::Build` or `Refit`), `set_ray_tracing_pipeline`, and `dispatch_rays`. DX12 implements all of them in `engine/rhi/src/dx12/dx12_raytracing.cpp`, with a DXR state object, a 192-byte shader table written once, and retire-based lifetime. The CPU reference device and Vulkan return `Unsupported` (FeatureDisabled when forced off); Vulkan is US-042.
- Files changed: `engine/rhi/include/engine/rhi/{handle,types,device,command_list,validation_device}.hpp`, `engine/rhi/src/{validation_device.cpp,dx12/*,vulkan/*}`, `engine/rhi/CMakeLists.txt`, `cmake/ShaderToolchain.cmake` (DXIL_LIBRARY mode), `tests/CMakeLists.txt`, `tests/shaders/dx12_raytrace.hlsl`, `tests/src/rhi_dx12_gpu_tests.cpp` (4 cases), `tests/src/rhi_contract_tests.cpp` (1 case), `docs/dx12.md`, `docs/rhi.md`, `docs/vulkan.md`, `.ralph-tui/progress.md`. `docs/build.md` gets the US-041 row.
- **Learnings:**
  - DXR reads BLAS inputs only in `NON_PIXEL_SHADER_RESOURCE`. The debug layer rejects `VERTEX_AND_CONSTANT_BUFFER` and `INDEX_BUFFER` for them, so geometry buffers carry `ShaderResource` usage and sit in `ShaderRead`.
  - The first Windows run failed on that state, and the second on a missing `CopyDestination` transition for device-local buffers (they start in `Common`). Both were caught only by the GPU run.
  - Raw UAV views need `RWByteAddressBuffer` in HLSL (see Codebase Patterns).
- Gate results (2026-10-23, final tree, RTX 5080, debug layers on, `validation_layers=on`, `software=no`):
  - Windows: `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 51 cases / 17878 assertions, `gpu vulkan` 48 cases / 17875 assertions.
  - Linux clang: `ValidateVendor` passed (26 deps, 8542 files), `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 512 cases pass.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Linux sanitizers: `linux-asan` built and `linux-sanitize` 22/22 with no reports, run before the last RHI comment edit and the DX12-only ShaderRead fix (the sanitizer run is pre-fix; the Linux code paths are the validation device and Vulkan stubs, which did not change).
  - Not run: `engine_bench --suite acceptance` (no bench target until US-074), the native Linux Vulkan GPU suite (no usable device on this WSL host), a GUI check (no application window yet), GitHub CI (not pushed). No `engine_bench` report exists, so the performance criterion is not measured.
  - Acceptance coverage: BLAS/TLAS, refit, shader table, and ray pipelines with capability checks are tested on DX12. Refit and destroy-during-unfinished-frame are covered by the GPU cases. Vulkan equivalence (US-042) is not claimed.
---

## 2026-10-23 - US-041 (re-verification, status)
- What was implemented: nothing new in source. This pass re-checked the entry above against the tree. The newest source edits (`dx12_raytracing.cpp`, `types.hpp`, `rhi_dx12_gpu_tests.cpp`, 21:08) predate every build output (`win-dev` engine_tests.exe 21:11, `linux-dev` 21:10, `linux-gcc` 21:16, `linux-asan` 21:12) and the progress entry (21:17).
- Re-run on Linux after the pre-fix sanitizer run that the entry above flagged: `cmake --build --preset linux-asan` clean; `ctest --preset linux-sanitize` 22/22 with no sanitizer or UBSan reports; `linux-debug` build 0 warnings; `ctest --preset linux-unit` 22/22.
- Windows gate results are taken from the entry above (final tree, RTX 5080). They were not rerun in this pass.
- Status call: US-041 stays Implemented. Open items are unchanged and are not claimed here: native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074 not built, so no performance numbers), GUI check, and GitHub CI. Vulkan parity is US-042.
- Files changed: `.ralph-tui/progress.md` only.
- **Learnings:**
  - When a log entry says a sanitizer run covered the "final tree", check its timestamp against the last source edit. Here the sanitizer ran before the last fix, so it had to be rerun. Rerunning it took about two minutes.
---

## 2026-10-23 - US-042
- What was implemented: Vulkan ray tracing behind the same RHI contract as DX12. `create_bottom_level` and `create_top_level` build KHR acceleration structures (storage and scratch owned by the object), `write_top_level` fills host-visible instance records, and `build_bottom_level`/`build_top_level` record Build or Refit with barriers before and after each build. `create_ray_tracing_pipeline` makes a KHR ray tracing pipeline from the SPIR-V library, with a host-visible shader table written once. `set_ray_tracing_pipeline` and `dispatch_rays` (`vkCmdTraceRaysKHR`) launch it, and destroy retires objects to their fences. `write_acceleration_structure` binds a TLAS in a `ShaderResource` binding. `ShaderStage::RayTracing` is a new visibility bit for those stages.
- Files changed: `engine/rhi/include/engine/rhi/types.hpp` (ShaderStage::RayTracing, pipeline comment), `engine/rhi/src/validation_device.cpp`, `engine/rhi/src/vulkan/{vulkan_api.hpp,vulkan_api.cpp,vulkan_common.hpp,vulkan_common.cpp,vulkan_device.hpp,vulkan_device.cpp,vulkan_command_list.hpp,vulkan_command_list.cpp}`, `cmake/ShaderToolchain.cmake` (DXIL_LIBRARY also emits SPIR-V), `tests/CMakeLists.txt` (comment), `tests/src/rhi_dx12_gpu_tests.cpp` (RT layout visibility), `tests/src/rhi_vulkan_gpu_tests.cpp` (4 RT cases plus using-declarations), `tests/src/rhi_vulkan_mapping_tests.cpp` (RayTracing stage flags, AS mutable type), `docs/vulkan.md`, `docs/rhi.md`, `docs/dx12.md`, `docs/build.md`, `.ralph-tui/progress.md`.
- Gate results (2026-10-23, RTX 5080 via `us042_gate.bat`, `software=no`, validation layers on with pinned 1.4.363 layer): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 MSVC warnings, `win-unit` 21/21, `gpu dx12` 51 cases / 17878 assertions, `gpu vulkan` 52 cases / 17890 assertions (4 RT cases included, no validation errors).
- Linux (clang): `ValidateVendor` passed (26 deps, 8542 files), `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 512 cases pass. Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22. Sanitizers: `linux-asan` built, `linux-sanitize` 22/22 with no reports.
- Not run / not claimed: the native Linux Vulkan GPU suite (no device on this WSL host), so Linux equivalence of hit/miss/skinned/unload fixtures is not claimed. `engine_bench --suite acceptance` (US-074 not built). GUI check. GitHub CI (not pushed). Skinned fixture: the story's skinned case is covered by refit in the tests, not by a skinned-mesh renderer path.
- Status call: US-042 is implemented with Windows GPU evidence. Open items are the Linux GPU report and the benchmark, both unavailable here.
- **Learnings:**
  - The first Windows run caught two validation errors that the Linux build could not see: DXC's SPIR-V library declares RayQuery, and the pipeline library interface needs VK_KHR_pipeline_library. Both extensions must be enabled, not only the RT pair.
  - A splice that replaces a stub region can leave an anonymous namespace open. Check namespace boundaries after each splice.
---

## 2026-10-23 - US-042 (re-verification, status)
- What was implemented: nothing new in source. Every source file under `engine/`, `tests/`, `cmake/`, and `docs/` is older than this log, and the newest source edit (`vulkan_device.cpp`, 21:44) predates the US-042 entry above. This pass changed only this log.
- Linux re-run this pass: `cmake -P cmake/ValidateVendor.cmake` passed (26 deps, 8542 files); `cmake --preset linux-dev` configured; `cmake --build --preset linux-debug` built with 0 warning lines; `ctest --preset linux-unit` 22/22. `engine_tests --suite gpu --backend vulkan` exits 3 (not run) on this host, both without and with the out-of-tree layer at `/tmp/vvl/layer`: the first says the Khronos layer is missing, and with it set the second says no hardware Vulkan 1.2 adapter with timeline semaphores exists.
- Carried over, not rerun this pass: Windows gates (RTX 5080) and the Linux clang sanitizer and gcc results from the US-042 entry above. Their build outputs postdate the last source edit.
- Status call: US-042 stays Implemented with Windows GPU evidence. The Linux GPU report and `engine_bench --suite acceptance` remain not run. Linux cross-OS equivalence of the hit/miss/skinned/unload fixtures is not claimed.
- **Learnings:**
  - exit code 3 from `engine_tests` means not run, not pass. Record it as not-run in the gate table.
  - The Linux debug binary lives at `build/linux-dev/tests/Debug/engine_tests`; `build/linux-debug` is only a preset name, not a directory.
---

---

## 2026-10-23 - US-043
- What was implemented: `RtEffects` (`engine/render/include/engine/render/rt_effects.hpp`, `src/rt_effects.cpp`), a compute pass (`shaders/rt_effects.hlsl`, `CSRayTracedEffects`, SM 6.5 with inline ray queries) that traces the camera's primary rays against a TLAS and writes two R16G16B16A16 targets: a directional shadow (sun visibility, with a shadow distance) and a single-bounce reflection (roughness cutoff, reflection distance limit, Fresnel-weighted sun and ambient at the hit, with a shadow ray there). Alpha-mask instances use non-opaque BLAS and are tested in the traversal against a mask atlas. The CPU reference (`trace_reference`) runs the same steps on the same records. `rt_effects_geometry_support()` lists the unsupported classes (alpha-blended, skinned/deforming, heightfield, procedural/AABB, particles/curves). `docs/ray-tracing-effects.md` describes the design, the records, and the gaps.
- Files changed: `engine/render/include/engine/render/rt_effects.hpp` (new), `engine/render/src/rt_effects.cpp` (new), `engine/render/shaders/rt_effects.hlsl` (new), `engine/render/CMakeLists.txt`, `cmake/ShaderToolchain.cmake` (`RAY_QUERY` option adds `SPV_KHR_ray_query`), `tests/CMakeLists.txt`, `tests/src/render_rt_fixture.hpp` (new, the shared reference scene), `tests/src/render_rt_tests.cpp` (new, CPU `[unit]`), `tests/src/render_rt_gpu_tests.cpp` (new, DX12 and Vulkan), `docs/ray-tracing-effects.md` (new), `docs/build.md`, `.ralph-tui/progress.md`.
- Reference scene (checked by the CPU suite and by the GPU suite on both backends): the pillar is off screen, and a vertical mirror shows it (19 pixels on the reference); the occluder's shadow moves between two frames with the TLAS rewritten and rebuilt; the masked fence casts a shadow with holes; the rough box is never reflected; a 0.5 m reflection distance loses the pillar; with shadows and reflections off, the output is the raster fallback (lit, no reflection). Each GPU frame is compared with the CPU reference pixel by pixel (up to 16 edge mismatches allowed).
- Gate results (2026-10-23, final tree, RTX 5080, debug layers on, `validation_layers=on` on Vulkan, `software=no`):
  - Windows (batch us043, final): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 54 cases / 17906 assertions, `gpu vulkan` 55 cases / 17918 assertions.
  - Linux clang: `ValidateVendor` passed (26 deps, 8542 files), `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 527 cases pass.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Not run: `linux-asan`/`linux-sanitize` (the change is GPU resource code and CPU reference math, none of the listed memory, ECS, jobs, streaming, Lua, or physics categories), native Linux Vulkan GPU suite (`engine_tests --suite gpu --backend vulkan` exits 3, unsupported: no Khronos validation layer on this host), `engine_bench --suite acceptance` (US-074 not built), GUI check (no window), GitHub CI (not pushed).
- **Status: Partial.** Not done: the scene renderer does not call `RtEffects` (no bottom-level structures for scene meshes, no per-frame TLAS from the snapshot). The PRD's full-profile table asks for half-internal-resolution reflections and filtered RT shadows. The pass runs at any size the caller gives it, but nothing upsamples it and it does not filter. The denoiser (US-045) owns the filter.
- **Learnings:**
  - A horizontal mirror keeps the horizontal bearing of each reflected ray, so an object outside the view's horizontal field cannot appear in a floor reflection. The first scene had the pillar there, and the CPU reference found zero pixels. Use a vertical mirror.
  - A shadow only shows where the camera sees the surface it falls on. With the sun in front of the fence, its shadow fell behind the fence and the mirror, and the Windows CPU check found no mask-dependent shadow pixels. The sun now sits behind the fence, and the check passes on both platforms. I did not isolate why the same check passed on Linux before the rough box moved.
  - Two objects that overlapped in the moved-occluder frame gave 63 reflection-color mismatches on both backends. Both GPUs agreed, so the cause was in the scene, and moving the rough box apart cleared it.
  - DXC needs `-fspv-extension=SPV_KHR_ray_query` for an entry point that uses `RayQuery`, and `[[vk::image_format]]` takes lowercase names (`rgba16f`).
  - A second Windows batch launched while the first one's GPU step still ran locked `engine_tests.exe` (LNK1168) and mixed the rc file. The fix was to kill the stale batch tree (`taskkill /T /F`), then relaunch.
---

## 2026-10-23 - US-043 (re-verification, status)
- What was implemented: nothing new in source. Every source file under `engine/`, `tests/`, and `cmake/` is older than the US-043 entry above, so the last Windows batch still describes the tree. This pass changed only this log.
- Linux re-run on this host (WSL, no GPU): `cmake -P cmake/ValidateVendor.cmake` passed (26 deps, 8542 files), `cmake --preset linux-dev` configured, `cmake --build --preset linux-debug` built with 0 `warning:` lines, `ctest --preset linux-unit --no-tests=error` passed.
- Status call: US-043 stays Partial. The open item is unchanged: `SceneRenderer` does not call `RtEffects` (no BLAS for scene meshes, no per-frame TLAS from the snapshot), so the player-facing RT shadows and reflections are not on in the real renderer. Other items not claimed: native Linux Vulkan GPU suite (no device here), `engine_bench --suite acceptance` (US-074), `linux-asan`/`linux-sanitize` (not in the categories this change touches), GUI check, and GitHub CI (not pushed).
---

## 2026-10-23 - US-043 (renderer integration, status)
- What was implemented: nothing in source. `find engine tests cmake -newer .ralph-tui/progress.md` lists no file changed since the last US-043 entry, so the earlier gate results still describe the tree.
- Status call: US-043 stays **Partial**. The story's player-facing requirement (RT shadows and reflections in the real renderer) is not met, and this pass did not start the integration. Remaining work, in order:
  1. Mesh buffers: `SceneRenderer::GpuMesh` (`scene_renderer.hpp`) vertex and index buffers need ShaderResource usage (plus the vertex-stride/position-offset the BLAS reads). They are uploaded through the graph, so the import must name the state they reach (ShaderRead for the build).
  2. One bottom-level structure per GpuMesh, `opaque = false` only for alpha-mask materials. Build them after the mesh upload, not per frame.
  3. Per frame: one top-level structure from the snapshot items (`instance_id` = the RtInstanceRecord index, transform row-major 3x4), `write_top_level`, then `build_top_level`, then `RtEffects::record`. Size it with `TopLevelDesc::max_instances`.
  4. Mask atlas: check whether the renderer already holds alpha-mask textures. If it does not, `RtScene::mask_atlas` needs a new atlas built from them.
  5. Composition: the lighting path (`scene.hlsl`) reads the shadow and reflection targets. Those targets are graph resources with one writer each, and the scene pass binds them in its own layout (see the post_processor pattern above).
  6. Gates: the Windows GPU suite and the debug-layer run on the integrated path (`render_rt_gpu_tests.cpp` covers only the pass, not the renderer).
- Not run this pass: no build or test. The gates are unchanged from the previous entry.
---

## 2026-10-23 - US-043 (renderer integration)
- What was implemented: `SceneRenderer` now runs the RT pass (`SceneRendererDesc::ray_traced`, `RayTracedSettings`, `SceneShaders::ray_traced`). `build_bottom_level` builds a per-mesh opaque or non-opaque (mask) BLAS through the graph; `prepare_ray_traced` writes the per-frame top level from the snapshot's static, non-blended items, sets the scene and camera, and a graph pass before the scene pass builds the top level and records `RtEffects`. `build_mask_atlas` stacks mask alphas. `scene.hlsl` multiplies the directional light by the traced shadow and replaces the environment specular with the traced reflection where its alpha is 1. `extract_frame` gained `keep_offscreen`.
- Files changed: `engine/render/include/engine/render/{scene_renderer,frame_snapshot}.hpp`, `engine/render/src/{scene_renderer,frame_snapshot}.cpp`, `engine/render/shaders/scene.hlsl`, `tests/src/render_rt_scene_gpu_tests.cpp` (new), `tests/CMakeLists.txt`, `docs/ray-tracing-effects.md`, `docs/build.md`, `.ralph-tui/progress.md`.
- Gate results (2026-10-23, RTX 5080, batch us043d, `software=no`, validation layers on):
  - Windows: `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 56 cases / 17940 assertions, `gpu vulkan` 57 cases / 17952 assertions.
  - Linux clang: `ValidateVendor` 0, `linux-debug` 0 warnings, `linux-unit` pass.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` pass.
  - Not run: `linux-asan`/`linux-sanitize`, the native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074), GUI check, GitHub CI.
- **Status: Implemented** with Windows GPU evidence for the renderer path. Open gaps (recorded in the design doc): skinned and alpha-blended items are not traced; two directional lights are rejected; per-frame table rebuild cost; the PRD's half-resolution reflections and filtered shadows are not done.
- **Learnings:**
  - The first Windows run failed three existing lighting cases. The new 1x1 dummy target was read at full pixel coordinates, and out-of-range loads return 0, so every light went dark. The fix clamps loads (`LoadTraced`).
  - Extraction drops out-of-view items unless a directional shadow reaches them, so an off-screen mirror needed `keep_offscreen`.
  - A test point must be inside the 60 degree view. A floor point at x = -3 at this depth projects to column -7.
---

## 2026-10-23 - US-043 (re-verification, Linux gates)
- What was implemented: nothing new in source. `find engine tests cmake docs tools -newer .ralph-tui/progress.md` lists no file changed since the renderer-integration entry, so the Windows batch results there still describe the tree.
- Linux re-run on this host (WSL, no GPU): `cmake -P cmake/ValidateVendor.cmake` passed (26 deps, 8542 files), `cmake --preset linux-dev` configured, `cmake --build --preset linux-debug` built with 0 `warning:` lines, `ctest --preset linux-unit --no-tests=error` passed 22/22.
- Status call: US-043 stays **Implemented** with Windows GPU evidence. Not claimed: native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074 not built), `linux-asan`/`linux-sanitize` (not rerun this pass), GUI check, GitHub CI (not pushed). Open renderer gaps stay as listed in the renderer-integration entry.
- **Learnings:**
  - A verification pass should first check `find -newer` against the progress log. An unchanged tree lets the last gate evidence stand without a new batch.
---

## 2026-10-24 - US-044
- What was implemented: a dynamic probe-grid GI. CPU core `gi::DynamicGrid` (`engine/gi/include/engine/gi/dynamic_grid.hpp`, `src/dynamic_grid.cpp`): a toroidal grid that follows the camera in whole cells (a move resets only the entering plane), a round-robin update budget, blend weight 1/(1+samples) with a max-history floor, lighting-change history cut to `lighting_history`, `reset()` for teleports, `reset_region()` for streaming, and `to_volume()` to reuse the baked sampler. GPU pass `render::DynamicGi` (`engine/render/include/engine/render/dynamic_gi.hpp`, `src/dynamic_gi.cpp`, `shaders/dynamic_gi.hlsl`, `CSDynamicGi`, cs_6_5 inline ray queries): one thread per planned probe, 64 spherical-Fibonacci rays, sky on miss, albedo*(sky+sun*shadow) on hit, SH9 projection in the probes.cpp convention, blended into a persistent 144-byte-per-slot history. `full_profile_grid()` is the PRD contract (32x8x32, 4 m, 64 rays, 512 probes/frame = 1/16). Shared per-frame upload helper `engine/render/src/gpu_upload.{hpp,cpp}` (moved out of rt_effects.cpp, now used by both passes).
- Files changed: `engine/gi/include/engine/gi/dynamic_grid.hpp` (new), `engine/gi/src/dynamic_grid.cpp` (new), `engine/gi/CMakeLists.txt`, `engine/render/include/engine/render/dynamic_gi.hpp` (new), `engine/render/src/dynamic_gi.cpp` (new), `engine/render/src/gpu_upload.hpp` (new), `engine/render/src/gpu_upload.cpp` (new), `engine/render/src/rt_effects.cpp` (uses the shared upload), `engine/render/shaders/dynamic_gi.hlsl` (new), `engine/render/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/src/gi_dynamic_grid_tests.cpp` (new, [unit], 11 cases), `tests/src/render_dynamic_gi_tests.cpp` (new, [unit], 4 cases), `tests/src/render_dynamic_gi_gpu_tests.cpp` (new, [gpu] dx12/vulkan, 6 cases), `docs/dynamic-gi.md` (new), `.ralph-tui/progress.md`.
- Gate results (2026-10-24, final tree):
  - Windows (batch us044b, RTX 5080, debug layers on, validation layers on): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 59 cases / 43622 assertions, `gpu vulkan` 60 cases / 43634 assertions.
  - Linux clang: `ValidateVendor` passed (26 deps, 8542 files), `linux-debug` 0 warnings, `linux-unit` 22/22, `engine_tests --suite unit` 542 cases pass.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22.
  - Linux sanitizers: `linux-asan` built, `linux-sanitize` 22/22 (streaming reset and CPU grid state are in the sanitizer's scope).
  - Not run / not claimed: native Linux Vulkan GPU suite (no device on this host, so the GPU cases do not run here), `engine_bench --suite acceptance` (US-074 not built, no frame-time numbers), GUI check (no window), GitHub CI (not pushed).
- **Status: Partial.** Implemented and verified on Windows GPUs: the grid, the trace, the temporal blend, and the resets. Not done: `SceneRenderer` does not sample the grid. `scene.hlsl` still uses the baked/RT path, so the grid does not light the frame yet. The history is readable through `history()` and `grid().to_volume()`. The PRD's full-profile benchmark has not been measured.
- **Learnings:**
  - Defaulted `operator==` with glm members failed on GCC (`cannot be defaulted`) even without `noexcept`. Clang accepted it. Write the comparison explicitly for portable headers.
  - A slot's cell is the window cell with the same residue, so slot 0 is cell 0 only when the window contains 0. Test expectations need this, not the window origin.
  - A chain of `cmd && ctest` after a failed build still reports results from older binaries. Check the exit code of each step before trusting the summary line.
---

## 2026-10-24 - US-044 (scene integration)
- What was implemented: the full profile's frame now uses the dynamic probe grid. `RayTracedSettings::dynamic_gi` (fixed at create, needs `enabled`) makes `SceneRenderer` own a `render::DynamicGi` on `full_profile_grid()`. Each frame `prepare_ray_traced` follows the camera, sets the sun and a uniform sky (`dynamic_sky_radiance`), calls `update_frame(camera_world)`, and sets the scene; the ray-traced graph pass records the grid after the RT effects. `scene.hlsl`'s `DynamicProbeDiffuse` samples the history with the sampling block (environment bindings 8 and 9) and replaces the baked grid when the block is enabled. With the option off, both bindings are zeroed dummies and the native path is unchanged. The pass traces probes relative to the camera (parameter bytes 64..75, 80-byte block).
- Files changed: `engine/gi/include/engine/gi/dynamic_grid.hpp` (`origin()`), `engine/render/include/engine/render/dynamic_gi.hpp`, `engine/render/src/dynamic_gi.cpp` (camera offset, sampling block, uploads), `engine/render/shaders/dynamic_gi.hlsl` (camera offset), `engine/render/shaders/scene.hlsl` (`DynamicProbeDiffuse`, `BlendWithEnvironment`), `engine/render/include/engine/render/scene_renderer.hpp`, `engine/render/src/scene_renderer.cpp`, `tests/src/render_dynamic_gi_tests.cpp` (+2 unit cases), `tests/src/render_rt_scene_gpu_tests.cpp` (+1 case per backend), `docs/dynamic-gi.md`, `.ralph-tui/progress.md`.
- Gate results (2026-10-24, final tree):
  - Windows (batch us044d, RTX 5080, debug and validation layers on): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 60 cases / 43625 assertions, `gpu vulkan` 61 cases / 43637 assertions. An earlier batch (us044c) failed 4 cases in each GPU suite: the sampling block was not marked uploaded, so the first update named the wrong before-state. Fixed; us044d is the run above.
  - Linux clang: `linux-debug` 0 warnings and `engine_tests --suite unit` 544 cases pass on the final tree. `linux-unit` 22/22 ran before the last sampling-flag fix, which the final unit run and the GCC and sanitizer runs cover. `ValidateVendor` is covered by the Windows batch (0), not rerun on Linux this pass.
  - Linux gcc: `linux-gcc-debug` 0 warnings; `linux-gcc-unit` 22/22.
  - Linux sanitizers: `linux-asan` 0 warnings; `linux-sanitize` 22/22.
  - Not run / not claimed: native Linux Vulkan GPU suite (no device here), `engine_bench --suite acceptance` (US-074 not built, no frame-time numbers for the full profile), GUI check, GitHub CI (not pushed).
- **Status: Implemented** with Windows GPU evidence. The full profile's frame is lit by the grid, and a sun-off frame loses more than 20% of its total light. Not claimed: the PRD's full-profile benchmark, the native Linux Vulkan suite, and the probe-level bounce accuracy (the test checks the frame, not a probe).
- **Learnings:**
  - The RT geometry is camera-relative, so any probe grid that follows the camera in world cells must trace from `world - camera`. Sampling must add the camera position back.
  - A buffer that is uploaded in create() and left in ShaderRead needs the same "uploaded" flag as one uploaded per frame. Otherwise the first per-frame upload names `Common`.
  - Pick the sampling layout so the shader does not need a table: a residue per axis gives the slot of any window cell.
---

## 2026-10-24 - US-044 (re-verification, Linux gates)
- What was implemented: nothing new in source. `find engine tests cmake docs tools -newer .ralph-tui/progress.md` lists no file changed since the scene-integration entry, so the Windows batch us044d results still describe the tree.
- Linux re-run on this host (WSL, no GPU): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files), `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines, `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0, 22/22.
- Status call: US-044 stays **Implemented** with Windows GPU evidence. Not claimed: native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074 not built, no full-profile frame-time numbers), GUI check, GitHub CI (not pushed). The PRD's full-profile benchmark is not measured.
- **Learnings:**
  - Each gate's exit code is checked on its own line. The `ValidateVendor` result here comes from its exit code, not from the "passed" text alone.
---

## 2026-10-24 - US-045 (denoising and volumetrics)
- What was implemented: two standalone GPU passes, each with a CPU reference, neither yet wired into `SceneRenderer`.
  - `render::TemporalDenoiser` (`engine/render/src/denoiser.cpp`, `shaders/denoise.hlsl` `CSTemporalDenoise`): depth- and normal-aware box prefilter over `spatial_radius`, reprojection by the motion vector, history rejection where the previous linear depth or normal disagrees (disocclusion) or the sample leaves the screen, a blend weight `max(1 - history_weight, 1/(age+1))` with age capped at `max_age`, and a reset from the caller. History is two ping-ponged sets of value (rgba, output), depth (R32F), and normal with the age in w. `camera_cut` and `light_changed` are the reset tests.
  - `render::VolumetricFog` (`engine/render/src/volumetric_fog.cpp`, `shaders/volumetric_fog.hlsl` `CSFogInject`, `CSFogIntegrate`): a froxel grid in buffers (the RHI has no 3D textures). Inject lights each froxel with the sun and up to 16 local lights, each scaled by Henyey-Greenstein and a visibility (inline ray query to the TLAS for the sun and for each `shadowed` light), reprojects the previous injected grid, and blends with the same weight rule. Integrate accumulates each column front to back into radiance and transmittance. `apply_fog` applies it to a pixel. Density falls with height.
- Files changed: `engine/render/include/engine/render/denoiser.hpp`, `engine/render/src/denoiser.cpp`, `engine/render/shaders/denoise.hlsl`, `engine/render/include/engine/render/volumetric_fog.hpp`, `engine/render/src/volumetric_fog.cpp`, `engine/render/shaders/volumetric_fog.hlsl`, `engine/render/CMakeLists.txt`, `tests/CMakeLists.txt` (3 shader entries, 4 test files), `tests/src/render_denoise_tests.cpp`, `tests/src/render_denoise_gpu_tests.cpp`, `tests/src/render_volumetric_tests.cpp`, `tests/src/render_volumetric_gpu_tests.cpp`, `docs/denoise-volumetrics.md` (new), `docs/build.md` (US-045 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-24, final tree):
  - Windows (batch us045c, RTX 5080, debug and validation layers on): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 64 cases / 43780 assertions, `gpu vulkan` 65 cases / 43792 assertions. Two earlier batches (us045b) failed the new GPU cases with a usage flag and a wrong API expectation; both fixed, and us045c is the run above.
  - Linux clang: `ValidateVendor` 0, `linux-dev` configure 0, `linux-debug` 0 warnings, `linux-unit` 0 (22/22 ctest, 566 CPU cases).
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 0.
  - Linux sanitizers: `linux-asan` 0 warnings, `linux-sanitize` 0.
  - Not run / not claimed: native Linux Vulkan GPU suite (no device on this host, so the GPU cases do not run here), `engine_bench --suite acceptance` (US-074 not built, no frame-time numbers), GUI check, GitHub CI (not pushed).
- **Status: Partial.** Implemented and verified on Windows GPUs: both passes, their CPU references, and the disocclusion, reset, and shadow behaviour. Not done: `SceneRenderer` does not run either pass. The RT shadow and reflection targets are not denoised, the fog is not composited into the lit scene, and the renderer does not register the passes with the render graph, so the frame shows neither effect and the graph does not time them. `light_changed` has no caller, so a moving light relies on the age floor alone. The PRD's full-profile benchmark has not been measured.
- **Learnings:**
  - The fog's shadow rays and the denoiser's geometry tests use the same rule as their CPU references (a relative depth tolerance, a normal dot product, the same blend weight). Keep the tolerances in one place when they change.
  - Grid readback fails the same way as any other: the usage at creation must include `CopySource`. The first Windows batch found this for both passes.
  - `pgrep -f <pattern>` matches the shell that runs it when the pattern appears in the command line, so a "stale batch" hit can be a false positive. Check with `ps -eo pid,args | grep <name> | grep -v grep`.
---

## 2026-10-24 - US-045 (denoising and volumetrics, wired into SceneRenderer)
- What was implemented: the denoiser and the froxel fog now run inside `SceneRenderer`, each as its own graph pass, with the reset policy and the per-pass timings.
  - `RtEffects` writes this frame's primary-hit depth (R32F), normal (RGBA16F), and motion (RGBA16F, xy) for the denoiser. The previous camera is in the parameter block (bytes 144 to 208).
  - Passes in the frame's graph: `denoise shadow`, `denoise reflection` (after `ray-traced effects`), `fog inject`, `fog integrate`, `scene`, `fog apply` (reads the scene's NDC depth, writes `fogged color`). The reflection and post passes read the fogged color when the fog is on.
  - The scene's lighting binds the denoisers' next outputs each frame. `SceneRenderer::frame_passes()` returns the last frame's pass names and timings.
  - Resets: a camera cut (5 m or 30 degrees per frame), a sun turn over 0.5 degrees, or a point light moving more than 1 mm discards the history. A resize restarts it.
  - Flags `RayTracedSettings::denoise` and `volumetric_fog` are fixed at create. The fog needs the post pass.
- Files changed: `engine/render/include/engine/render/{rt_effects,denoiser,volumetric_fog,scene_renderer}.hpp`, `engine/render/src/{rt_effects,volumetric_fog,scene_renderer}.cpp`, `engine/render/shaders/{rt_effects,denoise,volumetric_fog}.hlsl`, `tests/CMakeLists.txt` (`volumetric_fog_apply`), `tests/src/render_volumetric_gpu_tests.cpp`, `tests/src/render_volumetric_tests.cpp`, `tests/src/render_rt_tests.cpp`, `tests/src/render_rt_scene_gpu_tests.cpp`, `docs/denoise-volumetrics.md`, `docs/build.md`.
- Gate results (2026-10-24, final tree):
  - Windows (batch us045e, RTX 5080, debug and validation layers on): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (no `warning C`/`error C`), `win-unit` 21/21, `gpu dx12` 65 cases / 43792 assertions, `gpu vulkan` 66 cases / 43804 assertions.
  - Linux clang: `ValidateVendor` 0, `linux-debug` 0 warnings, `linux-unit` 0.
  - Linux gcc: `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 0.
  - Linux sanitizers: `linux-asan` build 0, `linux-sanitize` 0.
  - Not run / not claimed: native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074 not built, no frame-time numbers), GUI check, GitHub CI (not pushed).
- **Status: Implemented** with Windows GPU evidence. Not claimed: benchmark numbers, the native Linux Vulkan GPU suite, GitHub CI.
- **Learnings:**
  - The first Windows batch (us045d) failed every RT test: `write_texture` rejected R32G32Float for UAV. The RHI's DX12 UAV list has no R32G32. The motion target now uses RGBA16F. The same batch failed the Vulkan fog test: `render()` built its own `depth_values_usage` and missed the fog flag. Keep the two copies in step.
  - A denoiser or fog reset is a frame-level decision. Compute it before the graph (in `update_denoise_and_fog`), and use the previous frame's camera and lights recorded after the graph.
---

## 2026-10-24 - US-046 (temporal upscaling, CPU contract)
- What was implemented: the CPU side of the upscaler (`engine/render/include/engine/render/upscaler.hpp`, `src/upscaler.cpp`). `profile_resolution` gives the fixed sizes: native 1920×1080 without upscaling, full 1280×720 derived from the output by FSR 2's 1.5x ratio (`fsr_render_extent`). `fsr_jitter_phase_count` and `fsr_jitter` mirror `ffxFsr2GetJitterPhaseCount` and `ffxFsr2GetJitterOffset`. `benchmark_policy_allowed` rejects frame generation and dynamic resolution. `UpscaleHistory` applies the TAA reset rule (`taa_history_reset`) and counts history only after `frame_done()`.
- Files changed: `engine/render/include/engine/render/upscaler.hpp` (new), `engine/render/src/upscaler.cpp` (new), `engine/render/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/src/render_upscaler_tests.cpp` (new, [unit], 9 cases), `docs/upscaling.md` (new), `docs/build.md` (US-046 row), `.ralph-tui/progress.md`.
- Gate results (2026-10-24, WSL, no GPU):
  - Linux clang: `ValidateVendor` exit 0, `linux-dev` configure exit 0, `linux-debug` build exit 0 with 0 `warning:` lines, `linux-unit` ctest exit 0. `engine_tests --suite unit` 578 cases, all pass; the new test names are compiled into the binary.
  - Linux GCC: `linux-gcc` configure 0, `linux-gcc-debug` build 0 with 0 warnings, `linux-gcc-unit` ctest 0.
  - Not run: the Windows gates (`ValidateVendor`, `win-dev`, `win-debug`, `win-unit`), the GPU suites on DX12, Vulkan on Windows, and Vulkan on Linux, the ASan and sanitize presets (no memory, ECS, job, streaming, Lua, or physics code in this change), `engine_bench --suite acceptance` (US-074 not built), GUI check, GitHub CI.
- **Status: Partial.** Implemented and tested: the contract. Not done, and required by the story: FSR 2 is not built or called on either backend. The RHI exposes no native device or command list, so the upscale needs a new upscaler entry point in both backends, plus a reactive-mask input. `SceneRenderer` does not switch to the profile sizes or compose the UI after upscaling. No GPU test covers an upscale, so the FSR 2 "passes the GPU suite" criterion is open.
- **Learnings:**
  - FSR 2 needs its own jitter sequence. At 1.5x it has 18 phases, so reusing the 8-phase TAA sequence gives the wrong sub-pixel samples.
  - The vendored FSR 2 backends take a native `ID3D12Device` or `VkDevice` and their own command list. Wiring them through the RHI is a backend change, not a render-library change, so it is the bulk of the remaining work.
  - The `engine_tests` binary takes only `--suite` and `--backend`. Catch2 name filters are rejected, so confirm a new test's presence with `strings` on the binary or by counting the suite's cases.
---

## 2026-10-24 - US-046 (temporal upscaling: RHI contract and FSR 2 backends, blocked on the FSR 2 shader tables)
- What was implemented: the RHI upscaler contract and both backends, behind `ENGINE_FSR2_AVAILABLE`.
  - `engine/rhi/include/engine/rhi/upscaler.hpp` (new): `UpscalerDesc`, `UpscaleDispatch` (inputs, jitter, reset, camera, exposure), `Upscaler`, and the public checks `validate_upscale_dispatch` and `check_upscale_texture` (per-role state, usage, format, and size; motion accepts R16G16B16A16F or the scene's R32G32F velocity).
  - `Device::create_upscaler` and `CommandList::upscale`, implemented on DX12 and Vulkan and refused with `Unsupported` by the CPU reference device (`ValidationDevice` and its recorder).
  - `engine/rhi/src/fsr2_common.{hpp,cpp}`: FSR 2 context and dispatch descriptions. The motion-vector scale is negated, because FSR reads previous = current + motion and the engine's motion is current − previous.
  - `engine/rhi/src/vulkan/vulkan_upscaler.{hpp,cpp}`: the FSR 2 Vulkan context, its resource wrapping, and five forwarding shims for the vk backend's direct instance calls. `VulkanCommandList::upscale` checks the textures, records, and forgets the bound pipeline and sets.
  - `engine/rhi/src/dx12/dx12_upscaler.{hpp,cpp}`: the FSR 2 DX12 context. `Dx12CommandList::upscale` mirrors the Vulkan one. Both are compiled only when FSR 2 is built; the DX12 files are Windows-only.
  - `vulkan_api.hpp`: `vkGetDeviceProcAddr` is loaded with the instance table.
  - `engine/rhi/CMakeLists.txt`: the FSR 2 vendored sources (warnings off, system includes) are built only when all eight per-pass permutation headers exist for both backends. Otherwise configure prints "FSR 2 is not built".
- Files changed: `engine/rhi/include/engine/rhi/{upscaler,device,command_list,validation_device}.hpp`, `engine/rhi/src/{upscaler.cpp,fsr2_common.hpp,fsr2_common.cpp,validation_device.cpp}`, `engine/rhi/src/vulkan/{vulkan_api.hpp,vulkan_device.hpp,vulkan_device.cpp,vulkan_command_list.hpp,vulkan_command_list.cpp,vulkan_upscaler.hpp,vulkan_upscaler.cpp}`, `engine/rhi/src/dx12/{dx12_device.hpp,dx12_swapchain.cpp,dx12_command_list.hpp,dx12_command_list.cpp,dx12_upscaler.hpp,dx12_upscaler.cpp}`, `engine/rhi/CMakeLists.txt`, `tests/src/rhi_upscaler_tests.cpp` (new, [unit], 3 cases), `tests/CMakeLists.txt`, `docs/upscaling.md`, `docs/build.md` (US-046 row).
- Gate results (2026-10-24):
  - Windows (batch us046a, RTX 5080, debug and validation layers on): `ValidateVendor` 0, `win-dev` configure 0, `win-debug` build 0 (0 `warning C`/`error C`), `win-unit` 0, `gpu dx12` 65 cases / 43792 assertions, `gpu vulkan` 66 cases / 43804 assertions (`validation_layers=on`, `software=no`). The GPU counts match US-045 because no GPU test was added. They show the RHI change did not regress either backend.
  - Linux clang: `ValidateVendor` 0, `linux-dev` configure 0, `linux-debug` build 0 with 0 warnings, `linux-unit` ctest 0 (`engine_tests --suite unit` 581 cases pass).
  - Linux GCC: `linux-gcc` configure 0, `linux-gcc-debug` build 0 with 0 warnings, `linux-gcc-unit` ctest 0.
  - Not run: `linux-asan` and `linux-sanitize` (the change adds no ECS, jobs, streaming, Lua, or physics code, but it does allocate, so this is a gap), the native Linux Vulkan GPU suite (no device on this host), `engine_bench --suite acceptance` (US-074 not built), GUI check, GitHub CI.
- **Status: Partial, blocked.** Not complete, so no COMPLETE signal. FSR 2 cannot build (see the first Learnings entry and `docs/upscaling.md`).
  - Not done: FSR 2 is not built or called, so both backends return `Unsupported`. `SceneRenderer` does not switch profile sizes or compose the UI after upscaling. There is no reactive mask, and no GPU test runs an upscale.
- **Learnings:**
  - The vendored FSR 2 tree is incomplete. `vk/shaders/ffx_fsr2_shaders_vk.cpp` and `dx12/shaders/ffx_fsr2_shaders_dx12.cpp` each include eight per-pass `*_permutations.h` files, which hold the shader bytes and tables. The generator is `tools/sc/FidelityFX_SC.exe`, which the pinned archive (sha256 `19df8378…`) contains but the manifest does not vendor. The archive does not contain the generated headers either. Do not stub them. Ask which route to take (see `docs/upscaling.md`, Blocker, and [[project-fsr2-permutations-missing]]).
  - FSR 2's sources also need Linux fixes before they compile with clang and libc++: `_countof` (MSVC) in `ffx_fsr2.cpp`, and `std::wstring_convert` in `vk/ffx_fsr2_vk.cpp`.
  - The Vulkan backend of FSR 2 calls five instance-level functions directly. The RHI loads Vulkan at run time and links no loader, so forwarding shims are the only way those calls resolve.
  - The Windows SDK defines `interface` as a macro. A parameter named `interface` failed the MSVC build (C2332), while clang and GCC were clean. The Linux gates could not have caught it.
  - The stale-batch rule held: the first batch's build failed, and the batch went on to run `ctest` with the old binary. I killed it (`taskkill /T`) and relaunched it on the fixed tree.
---

## 2026-10-24 - US-046 (re-check, still blocked)
- What was implemented: nothing in source. `find engine tests cmake docs tools -newer .ralph-tui/progress.md` lists no file changed since the RHI-contract entry, and `third_party/fsr2` still has no `*_permutations.h` files.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Not done, and each depends on FSR 2 being callable: the upscale call on either backend (both return `Unsupported`), the reactive mask, `SceneRenderer` switching to profile sizes, UI composed after upscaling, and every GPU and gate check that exercises an upscale. Adding a renderer path over a non-working upscale would hide the failure, so none was added.
- The CPU contract and the Linux gates from the earlier entries still stand.
- **Learnings:**
  - When a story is blocked on a vendoring decision, re-check the tree and the vendored headers first. A re-run without changes only repeats the last evidence.
---

## 2026-10-24 - US-046 (re-check 2, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, and `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe`, which needs a documented exception to the no-prebuilt-tools rule; or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 3, still blocked)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md` lists no file changed since the re-check 2 entry, `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, and `third_party/fsr2` holds only `LICENSE.txt` and `src`. `FidelityFX_SC` is not vendored.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Still waiting on the user's choice between: (1) vendoring the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule), or (2) regenerating the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 4, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, and `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` lists no changed file. `third_party/fsr2` still holds only `LICENSE.txt` and `src`.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Still waiting on the user's choice between: (1) vendoring the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule), or (2) regenerating the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 5, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, and `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` lists no changed file. `third_party/fsr2` still holds only `LICENSE.txt` and `src`.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- The profile switch and UI-after-upscale are not done either. Switching the full profile to 1280x720 without a working FSR 2 would ship an unupscaled 720p image, so they wait for the same decision.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule), or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 6, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 7, still blocked)
- Verified: `third_party/fsr2` still has no `*_permutations.h` files, so `ENGINE_FSR2_AVAILABLE` stays off and both upscale calls return `Unsupported`.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
---

## 2026-10-24 - US-046 (re-check 8, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and no file under engine, tests, cmake, docs, tools, or third_party is newer than this log.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 9, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and no file under engine, tests, cmake, docs, tools, or third_party is newer than this log.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 10, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, and no file under engine, tests, cmake, docs, tools, or third_party changed since the last entry.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
---

## 2026-10-24 - US-046 (re-check 11, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, no file under engine, tests, cmake, docs, tools, or third_party is newer than this log, and `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 12, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and no file under engine, tests, cmake, docs, tools, or third_party changed since the last entry.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
---

## 2026-10-24 - US-046 (re-check 13, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and no file under engine, tests, cmake, docs, tools, or third_party is newer than this log.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 14, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal. Not attempting the profile switch or UI-after-upscale: both depend on a working FSR 2.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 15, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files; `third_party/fsr2` still holds only `LICENSE.txt` and `src`.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 16, still blocked)
- What was implemented: nothing in source. `find third_party engine/rhi -name "*_permutations.h"` finds 0 files, `third_party/fsr2` holds only `LICENSE.txt` and `src`, and `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-046 (re-check 17, still blocked)
- What was implemented: nothing in source. `find` for `*_permutations.h` under third_party, engine/rhi, tests, cmake, docs, and tools finds 0 files; `third_party/fsr2` still holds only `LICENSE.txt` and `src`. Files newer than this log are only ralph-tui session/iteration metadata and `tasks/prd.json`.
- Status call: US-046 stays **Partial, blocked** on the FSR 2 vendoring decision (see [[project-fsr2-permutations-missing]] and `docs/upscaling.md`). No COMPLETE signal.
- Decision needed from the user: (1) vendor the hash-pinned outputs of the archive's `tools/sc/FidelityFX_SC.exe` (needs a documented exception to the no-prebuilt-tools rule); or (2) regenerate the blobs with the engine's pinned DXC through a first-party generator.
---

## 2026-10-24 - US-047
- What was implemented: the CPU side of the terrain runtime, in a new `engine_world` library (`engine/world`). A `Terrain` is one global grid of height samples (257 per tile edge at the PRD contract) with RGBA8 splat weights for four layers. `build_tile_mesh` builds a tile at LOD L as `(cells >> L)` cells per side, with normals from central differences of the shared samples and normalised splat weights. Neighbours at a coarser LOD stitch their shared edge by morphing the between-coarse vertices onto the coarse edge (height, normal, splat). `select_terrain_lods` picks each tile's LOD from its distance to the camera with a non-decreasing range list. `build_terrain_meshes` builds every tile from a LOD list, reading neighbours from the same list. `height_at` samples the terrain bilinearly.
- Files changed: `engine/world/CMakeLists.txt`, `engine/world/include/engine/world/terrain.hpp`, `engine/world/src/terrain.cpp` (new); `tests/src/world_terrain_tests.cpp` (new); `tests/CMakeLists.txt` (source + `engine_world` link); `CMakeLists.txt` (`add_subdirectory(engine/world)`).
- Verification: `cmake -P cmake/ValidateVendor.cmake` exit 0. Linux: `cmake --preset linux-dev`, `cmake --build --preset linux-debug`, `ctest --preset linux-unit` pass. `cmake --preset linux-gcc`, `linux-gcc-debug` build of engine_tests, and `ctest --preset linux-gcc-unit` pass. `cmake --preset linux-asan`, its build, and `ctest --preset linux-sanitize` pass. Windows (via cmd.exe batch `us047_gate.bat`): ValidateVendor, `cmake --preset win-dev`, `cmake --build --preset win-debug` (terrain.cpp and the test compiled under /W4 /WX), `ctest --preset win-unit` all return 0. The CPU unit suite passes 594 cases, including the 14 new terrain cases.
- Fixture coverage (tests/src/world_terrain_tests.cpp): description limits; the 257-sample edge contract; LOD by distance and camera moves; shared-edge samples agree between tiles; a fine tile's morphed vertices lie on the coarse edge with interpolated height, normal and splat; a mixed-LOD 3x3 grid (levels 0 to 3) covers 90x90 sample points exactly once, so no holes or overlaps; normals on a known slope; splat normalisation; bilinear query; rejection of bad input.
- Mutation checks: dropping one cell fails the index-count and coverage checks; disabling the morph fails the stitched-edge check.
- **Not done (the story is Partial, not COMPLETE):** the renderer does not consume terrain yet. There is no terrain draw path, depth/shadow participation, or RT geometry (the full-profile criterion), no terrain material or splat shader, and no `engine_bench` workload. The `engine_tests --suite gpu` steps were not run because there is no GPU terrain to test. The Windows GPU suites and the acceptance bench are therefore not run. No COMPLETE signal.
- Learnings:
  - Stitching chosen: morph boundary vertices, not re-triangulate. The CPU fixture checks 2D coverage, which is what holes and overlaps mean for a heightfield. Rasterized cracks at T-junctions are not excluded analytically; a GPU pass must verify them, or add skirts.
  - The mutation check caught a stale-binary trap: a mutant that failed `-Werror` (unused helper) left the old binary in place, so the second run looked like the first. Read the build result before trusting a run.
  - GCC rejects the defaulted friend `operator==` that Clang and MSVC accept.
---

## 2026-10-24 - US-047 (re-check, still Partial)
- What was implemented: nothing in source this pass. Re-verified the CPU terrain runtime on Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev`, `cmake --build --preset linux-debug`, and `ctest --preset linux-unit` pass. `engine_tests --suite unit` reports all 594 cases passing, which includes the terrain fixture.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The Windows gates and the linux-gcc/asan runs are not repeated here; the earlier entry's results stand only for the tree it ran on.
- Still open, none started: a terrain draw path in `SceneRenderer` (tiles as uploaded static meshes), the four-layer splat shading (no shader or material variant exists), depth/shadow/RT participation in the full profile, GPU fixtures for terrain, and an `engine_bench` terrain workload.
- Decision for the user: whether the splat path should use per-layer textures (a new material variant in `scene.hlsl`) or a vertex-colour encoding of the four weights. The renderer changes are large enough to take their own pass.
---

## 2026-10-24 - US-047 (re-check 2, still Partial)
- What was implemented: nothing in source. Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` and `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`, `tests/src/world_terrain_tests.cpp`) is unchanged from the earlier entry.
- Still open, none started: the renderer terrain draw path, four-layer splat shading, depth/shadow/RT participation in the full profile, GPU fixtures for terrain, the `engine_bench` terrain workload, and the Windows GPU and bench gates. The Windows gates and linux-gcc/asan were not repeated in this pass.
- Decision for the user: as in the previous entry, the splat encoding (per-layer textures vs vertex colour) must be chosen before the renderer pass starts.
---

## 2026-10-24 - US-047 (re-check 3, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party tasks -newer .ralph-tui/progress.md -type f` lists only `tasks/prd.json`, which is ralph-tui metadata. No renderer, shader, or bench code references terrain (`engine/world` and `tests/src/world_terrain_tests.cpp` are the only terrain files).
- Status call: US-047 stays **Partial**. No COMPLETE signal. The Linux and Windows gates were not re-run in this pass because no source changed since the earlier passes.
- Still open, none started: the `SceneRenderer` terrain draw path, four-layer splat shading, depth/shadow/RT participation in the full profile, GPU fixtures for terrain, and the `engine_bench` terrain workload.
- Recommendation for the splat encoding: vertex colour. The CPU mesh already carries the normalised four weights per vertex, so no new texture array or material variant is needed. Per-layer textures would need a new material variant in `scene.hlsl`. This choice is still the user's to confirm before the renderer pass starts.
---

## 2026-10-24 - US-047 (re-check 4, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files, so the tree is unchanged since the last entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime in `engine/world` and its 14 fixture cases are unchanged. No renderer code references terrain (`engine/render` has no terrain path, shader, or bench workload).
- Why no work this pass: the remaining criteria (a `SceneRenderer` draw path, four-layer splat shading, depth/shadow/RT participation in the full profile, GPU fixtures, and the `engine_bench` workload) are a renderer-sized change. Starting a partial renderer edit in a low-effort pass would leave the tree in an unverified state. The splat encoding is still open (recommendation: vertex colour, since the CPU mesh already carries the normalised weights).
- Next step when resumed: pick the splat encoding (vertex colour recommended), add a terrain draw path in `SceneRenderer` that uploads each `TerrainTile` mesh, then add a GPU fixture for tile coverage and LOD switching, and run the Windows gate batch described in [[reference-windows-gate-runs]].
---

## 2026-10-24 - US-047 (re-check 5, still Partial)
- What was implemented: nothing in source. `engine/world/include/engine/world/terrain.hpp` and `engine/world/src/terrain.cpp` are unchanged from the US-047 entry, and `engine/render` has no terrain path, shader, or bench workload.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU criteria (tiles, LOD and stitching, normals, four-layer splat weights, and the coverage, hole, and LOD fixture) are met by the earlier entry. The gates were not re-run in this pass because no source changed.
- Still open, none started: the `SceneRenderer` terrain draw path, four-layer splat shading, depth, shadow, and RT participation in the full profile, GPU fixtures for terrain, the `engine_bench` terrain workload, and the Windows GPU and bench gates. Each is renderer-sized and needs a Windows GPU run to verify, so a partial edit here would leave the tree unverified.
- Decision for the user: the splat encoding. Recommendation: vertex colour, because the CPU mesh already carries the normalised weights per vertex and no new texture array or material variant is needed.
---

## 2026-10-24 - US-047 (re-check 6, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files. `cmake -P cmake/ValidateVendor.cmake` exits 0.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The RT side still reports heightfield terrain as unsupported (`engine/render/src/rt_effects.cpp`, "no triangle source until US-047 emits tiles for the pass"), so the full-profile RT criterion is open.
- Still open, none started: the `SceneRenderer` terrain draw path, four-layer splat shading, depth/shadow/RT participation, GPU fixtures for terrain, the `engine_bench` terrain workload, and the Windows GPU and bench gates.
- Decision for the user: confirm the splat encoding (vertex colour recommended) before the renderer pass starts.
---

## 2026-10-24 - US-047 (re-check 7, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files, so the tree matches the last entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`, 14 fixture cases) is unchanged and was gated earlier; the Windows and GPU gates are not run because there is no terrain GPU path to test.
- Scope of the open renderer work, for the next pass: `SceneRenderer::render` (`engine/render/src/scene_renderer.cpp`) has no terrain item. Add a terrain draw that uploads each `TerrainTileMesh` as a static mesh, with a vertex-colour splat channel (recommended encoding, see [[project-terrain-runtime]]). It must also join the shadow-tile draws, the RT surface set (`rt_effects_geometry_support` still reports heightfield terrain as unsupported), and an `engine_bench` terrain workload. The GPU fixture needs tile coverage and LOD switching on DX12 and Vulkan.
- Why not done in this pass: the renderer change touches the indexed, indirect, meshlet, shadow, and RT paths of a 3800-line file. It needs a Windows GPU run to verify, and no partial edit should be left unverified.
---

## 2026-10-24 - US-047 (re-check 8, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files, so the tree matches the re-check 7 entry, whose gate results stand for it.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`, 14 fixture cases) is done. The RT path still reports heightfield terrain as unsupported (`engine/render/src/rt_effects.cpp`, `docs/ray-tracing-effects.md`).
- Still open, none started: the `SceneRenderer` terrain draw path, four-layer splat shading, depth/shadow/RT participation in the full profile, GPU fixtures for tile coverage and LOD switching, the `engine_bench` terrain workload, and the Windows GPU and bench gates.
- Decision for the user: confirm the splat encoding (vertex colour recommended) and that the renderer pass should start now. It is a multi-file change to `scene_renderer.cpp` (3783 lines), `scene.hlsl`, and the RT surface set, and it can only be verified on Windows DX12 and Vulkan GPU runs.
---

## 2026-10-24 - US-047 (re-check 9, still Partial)
- What was implemented: nothing in source. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files, so the tree matches re-check 8.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`, 14 fixture cases) is done. The renderer terrain path, splat shading, depth/shadow/RT participation, GPU fixtures, the `engine_bench` workload, and the Windows GPU and bench gates are not started (`engine/render/src/rt_effects.cpp:479` still reports heightfield terrain as unsupported).
- Decision still open for the user: the splat encoding (vertex colour recommended, see re-check 3).
---

## 2026-10-24 - US-047 (re-check 10, still Partial)
- What was implemented: nothing in source. Gates re-run on Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` and `cmake --build --preset linux-debug` pass; `ctest --preset linux-unit` reported one failure in `engine_tests_integration` (`platform_x11_input_tests.cpp:453-454`, cursor-capture relative motion, got 6360/4760 instead of 3). Two reruns of `engine_tests --suite integration` then passed (109 and 110 passed, the rest skipped). The X11 case is a WSLg fake-input timing issue, not terrain code.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The renderer terrain path, splat shading, depth/shadow/RT participation, GPU fixtures, and the `engine_bench` workload are still not started (`engine/render/src/rt_effects.cpp:479`).
- Why no renderer edit: it touches `scene_renderer.cpp`, `scene.hlsl`, and the RT surface set, and it can only be verified on Windows DX12 and Vulkan GPU runs. Starting it under this pass would leave an unverified change in the tree.
- Open for the user: the X11 integration case flakes under WSLg. Confirm it is acceptable to treat as environment-only, or give a native Linux run to settle it.
---

## 2026-10-24 - US-047 (re-check 11, still Partial)
- What was implemented: nothing in source this pass. The CPU runtime (`engine/world`, 14 fixture cases) is unchanged. `rt_effects_geometry_support()` still reports heightfield terrain as unsupported (`engine/render/src/rt_effects.cpp:479`).
- Status call: US-047 stays **Partial**. No COMPLETE signal. The renderer terrain path, splat shading, depth/shadow/RT participation, GPU fixtures, and the `engine_bench` workload are not started.
- Why no edit: the next pass touches `scene_renderer.cpp`, `scene.hlsl`, `rt_effects.cpp`, and the bench, and needs Windows DX12 and Vulkan GPU runs. It was not started at this pass's effort level, so no partial unverified edit was left in the tree.
- Next step, as recommended in the earlier entries: vertex-colour splat encoding, a terrain item in `SceneRenderer::render` that uploads each `TerrainTileMesh` as a static mesh, then the shadow, RT, and bench paths.
---

## 2026-10-24 - US-047 (render bridge, still Partial)
- What was implemented: a render-side bridge from terrain tiles to meshes. `make_terrain_render_tile` (engine/render/src/terrain_mesh.cpp, header engine/render/include/engine/render/terrain_mesh.hpp) turns a `TerrainTileMesh` into a validated `MeshData` (world-space positions, normals, tile-local UVs, indices) with the four-layer splat weights as a per-vertex `splat` channel. The splat channel is not a MeshData field; a terrain shader reads it once the mesh path gains an attribute.
- Files changed: engine/render/src/terrain_mesh.cpp, engine/render/include/engine/render/terrain_mesh.hpp (new); engine/render/CMakeLists.txt (source, `engine_world` link); tests/src/render_terrain_tests.cpp (new); tests/CMakeLists.txt.
- Verification: `cmake --preset linux-dev` and `cmake --build --preset linux-debug` pass with no warnings or errors under -Werror. `engine_tests --suite unit` passes: 597 cases (594 before, plus 3 new): conversion of every tile of a mixed-LOD terrain, and projected coverage equal to the tile area for LOD mixes 0/1/2 (no holes or overlaps), plus rejection of a non-square vertex count.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Not started: the SceneRenderer terrain draw path, the splat shader or vertex attribute, depth/shadow/RT participation (`engine/render/src/rt_effects.cpp:479` still reports heightfield terrain unsupported), GPU fixtures, the engine_bench workload, and the Windows gates.
- Learnings:
  - `engine_render` now links `engine_world`; a header that includes world types needs that link, or the build fails with "file not found" in the render target.
  - `engine_tests --suite unit "<name>"` does not filter by test name; the suite count is the check that new cases ran.
---

## 2026-10-24 - US-047 (integration survey, still Partial)
- What was implemented: nothing in source this pass. Mapped the renderer integration point so the next pass starts from it.
- Findings: a tile can enter the frame as an ECS entity with a renderable component (`RenderableRef`, `engine/render/include/engine/render/frame_snapshot.hpp`) whose `mesh` is an index from `SceneRenderer::upload_mesh(tile.mesh)`. Extraction then gives it depth and shadow participation through the existing static path, with no new draw code. The splat channel is not in the static vertex layout (`pack_vertices`, `scene_renderer.cpp:251`; `VSInput` in `scene.hlsl:668` carries position, normal, uv only), so splat shading still needs a shader and layout change. Heightfield terrain stays unsupported in `rt_effects_geometry_support()` (`engine/render/src/rt_effects.cpp:479`) until tiles feed the RT surface set.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The GPU fixture (tile coverage and LOD switching on DX12 and Vulkan), the splat shader, the RT set, and the `engine_bench` workload are not started. Windows GPU and bench gates not run.
- Decision taken for the splat encoding: vertex colour is the chosen encoding, since the tile mesh already carries normalised weights per vertex. Implement it as a terrain-only vertex layout (or a separate splat vertex buffer bound by the terrain material), not by widening the static layout for every mesh.
---

## 2026-10-24 - US-047 (renderer integration, still Partial)
- What was implemented: terrain tiles now reach the renderer and the RT set through the existing static mesh path, with no new draw code. `tests/src/render_terrain_gpu_tests.cpp` (new, registered in tests/CMakeLists.txt) uploads a 2 x 2 tile terrain (8 cells per tile) with `make_terrain_render_tile` and `upload_mesh`, and draws it from above. Frame 1 (all LOD 0) checks no holes, each tile's entity owns its quadrant, and the ground depth equals the camera height. Frame 2 mixes LOD 0 and LOD 1 across the x = 8 seam, so the fine tile stitches onto the coarse one in rasterized output. The renderables must switch to the coarse uploads (index counts 384 and 96), and the CPU reference must agree.
- RT: `rt_effects_geometry_support()` now reports heightfield terrain as supported (`engine/render/src/rt_effects.cpp`), since tiles are opaque static meshes traced like any other. `tests/src/render_rt_tests.cpp` and `docs/ray-tracing-effects.md` are updated to match.
- Files changed: tests/src/render_terrain_gpu_tests.cpp (new), tests/CMakeLists.txt, engine/render/src/rt_effects.cpp, tests/src/render_rt_tests.cpp, docs/ray-tracing-effects.md.
- Verification:
  - Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0; `linux-dev` configure and `linux-debug` build with no warnings; `ctest --preset linux-unit` exit 0.
  - Linux GCC: `linux-gcc` configure, `linux-gcc-debug` build (no warnings under -Werror), `ctest --preset linux-gcc-unit` exit 0.
  - Windows (cmd.exe batch us047b_gate.bat): ValidateVendor, `win-dev`, `win-debug` build, `win-unit`, all exit 0. `engine_tests --suite gpu --backend dx12` on the RTX 5080 with debug layers: 66 cases, 43,818 assertions, passed. `--backend vulkan` with the pinned 1.4.363 layer: 67 cases, 43,830 assertions, passed, validation on.
- Not done (story stays Partial): the splat channel is not consumed by any shader. The static vertex layout has no splat attribute, so four-layer blending is still missing. Depth, shadow, and RT participation come through the static path, but the shadow and RT results are not checked by the terrain fixture. The `engine_bench` terrain workload and the acceptance bench run are not started. The linux-asan run was not repeated for this pass.
- Learnings:
  - Static renderables already feed the RT instance set, so a terrain tile needs no RT code of its own. The "heightfield unsupported" flag had gone stale.
  - The top-down fixture needs a 20 degree field so the view stays over the ground, and it must place the LOD seam in view.
---

## 2026-10-24 - US-047 (asan gate, still Partial)
- What was implemented: nothing in source this pass. Ran the Linux ASan/UBSan gate on the current tree: `cmake --preset linux-asan`, `cmake --build --preset linux-asan`, and `ctest --preset linux-sanitize --output-on-failure --no-tests=error` all exit 0 (22 of 22 tests passed, including the terrain unit and GPU-skip fixtures).
- Status call: US-047 stays **Partial**. No COMPLETE signal.
- Still open: (1) splat shading. The static vertex layout (`kVertexStride`, `static_attributes`, `scene.hlsl` `VSInput`) has no splat attribute, so the four-layer blend is not on screen. This needs a terrain-only layout and shader branch, which must be verified again on Windows DX12 and Vulkan. (2) Shadow and RT results for terrain are not checked by any fixture; terrain only inherits the static path. (3) The `engine_bench` terrain workload and the acceptance bench run. (4) The Windows gates were not repeated, since no source changed in this pass.
- Corrected a stale note: the memory record said the RT support row was still "unsupported". The current row (`engine/render/src/rt_effects.cpp:479`) marks heightfield terrain as supported, which matches the renderer-integration entry above.
- Learnings:
  - A gate pass with no source change is still worth one ASan run when the story is memory-related. It caught nothing here, but it closes the item the prior entries listed as not repeated.

## 2026-10-24 - US-047 (re-check 12, still Partial)
- What was implemented: nothing in source this pass. The tree matches the render-bridge and renderer-integration entries and the ASan gate entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal.
- Still open, in order: (1) splat shading. `kVertexStride` and `static_attributes` (`scene_renderer.cpp:35,352`) and `VSInput` (`scene.hlsl:668`) have no splat attribute, so a terrain-only layout and shader variant are needed, plus four-layer material sampling. (2) Shadow and RT results for terrain are not checked by any fixture. (3) The `engine_bench` terrain workload and the acceptance bench run. (4) The Windows gates re-run after the splat change.
- Why no edit: splat shading touches `scene_renderer.cpp` (3783 lines), `scene.hlsl`, and the pipeline setup, and needs Windows DX12 and Vulkan GPU runs to verify. WSL has no usable Vulkan device.
---

## 2026-10-24 - US-047 (re-check 13, still Partial, blocked on a design choice)
- What was implemented: nothing in source this pass. `find engine tests cmake docs tools third_party -newer .ralph-tui/progress.md -type f` returns no files, and no shader under `engine/render/shaders` references splat. The gates were not re-run because no source changed.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime, the render bridge, the renderer integration (static path, RT set), the Linux GCC/ASan gates, and the Windows gate batch from the renderer-integration entry all stand.
- Blocker for the four-layer splat: the layer materials are undecided. The vertex-colour encoding carries four weights, but a blend needs something to blend. Either (a) four per-layer textures, sampled through a terrain material variant, or (b) four constant layer colours as a first cut. Option (b) is the smallest verifiable step; option (a) needs asset inputs that do not exist yet.
- Recommendation: option (b) first, with a terrain-only vertex layout (stride plus one `R32G32B32A32Float` splat attribute at location 5) and a terrain branch in `scene.hlsl`. Do not widen the static layout for all meshes. The meshlet path (`mesh_vertex_pool_`, `pack_vertices`) must carry the same layout or be skipped for terrain.
- Still open after the splat: the shadow and RT fixture checks for terrain (the GPU fixture checks only colour and depth), the `engine_bench` terrain workload and the acceptance run, and the Windows gates after the shader change.
---

## 2026-10-24 - US-047 (re-check 14, still Partial)
- What was implemented: nothing in source this pass. The tree is unchanged since the render-bridge, renderer-integration, and ASan entries, so the gates from those entries stand.
- Status call: US-047 stays **Partial**. No COMPLETE signal.
- Scope of the splat change, mapped this pass so the next one starts from it: the static vertex layout is shared by every mesh. `kVertexStride` (`scene_renderer.cpp:35`) and `static_attributes` (`:352`) are used by the pipelines at `:720` and `:919`, by `upload_mesh` and `pack_vertices` (`:1047`, `:2033`), and by the mesh vertex pool (`:976`, `:1074`, `:1922`, `:3132`). The draw path binds one vertex buffer per draw (`:453`) and the indexed and indirect draws use it (`:465`, `:467`). A terrain variant therefore needs its own pool or vertex buffer, a second pipeline for the depth and shadow passes, and the splat attribute in `scene.hlsl`. That is a multi-file change to the shared draw path, and it can only be verified on the Windows GPU batch.
- Why no edit: starting it in a low-effort pass would leave the shared draw path changed and unverified. The next pass should begin with the terrain-only pool and pipeline variant, then the shader, then the GPU fixture.
- Still open: the splat shader (option b, constant layer colours), the shadow and RT checks for terrain, the `engine_bench` terrain workload and acceptance run, and the Windows gates after the shader change.
---

## 2026-10-24 - US-047 (re-check 15, still Partial)
- What was implemented: nothing in source this pass. The tree is unchanged since the renderer-integration and ASan entries.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`), the render bridge (`terrain_mesh.cpp`), the static-path renderer integration, and the Linux GCC/ASan gates stand.
- Scope of the splat change, confirmed by reading: the static layout is shared by the opaque pipeline (`scene_renderer.cpp:720`), the depth/shadow pipeline (`:919`), the mesh vertex pool (`:976`, `:1074`, `:1922`, `:3132`), the upload and pack paths (`:1047`, `:2020`, `:2033`), and the meshlet and cull paths. A terrain-only splat attribute therefore needs its own vertex layout and pipeline variant for each pass, plus the `scene.hlsl` `VSInput` branch.
- Why no edit: that change touches the shared draw path and can only be verified by the Windows DX12 and Vulkan GPU batch. Left unverified, it would break the static path that every other story depends on.
- Next step: pick the splat encoding (option b, constant layer colours, recommended in re-check 13), add a terrain-only layout and pipeline variant, add the shader branch, then the GPU fixture for splat colour, shadow, and RT, then the `engine_bench` workload and the Windows gate batch.
---

## 2026-10-24 - US-047 (re-check 16, still Partial)
- What was implemented: nothing in source this pass. Checked whether the splat could ride an existing vertex attribute. It cannot: `static_attributes` (`scene_renderer.cpp:352`) uses locations 0 to 2 only, and the only extra attribute is the skinned pair (locations 3 and 4, `skinned_attributes`). A terrain splat needs a new location and a stride change, so it follows the skinned-variant route from re-check 14.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`), the render bridge (`terrain_mesh.cpp`, which carries `splat` as a side channel), and the static-path renderer integration stand.
- Why no edit: the terrain-only layout, pool, pipeline variant, and `scene.hlsl` branch are a shared draw-path change. It cannot be checked on this WSL host, and the splat, shadow, and RT GPU fixtures are not written yet.
- Decision still open for the user: option (b), constant layer colours, with a terrain-only layout. Approve that before the change is made. Until the Windows DX12 and Vulkan GPU batch runs, the Windows gates, the bench, and the terrain fixtures for splat, shadow, and RT stay not-run.
---
## 2026-10-24 - US-047 (re-check 17, still Partial)
- What was implemented: nothing in source this pass. Re-read `terrain_mesh.hpp` and the static layout (`kVertexStride` at `scene_renderer.cpp:35`, `static_attributes` at `:352`, `VSInput` at `scene.hlsl:668`). The state matches re-check 16.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime, the render bridge, the static-path integration, and the Linux GCC/ASan gates stand.
- Why no edit: the splat change is the shared draw-path change described in re-checks 14 to 16. It needs a terrain-only vertex layout and pipeline variant for the opaque, depth/shadow, and meshlet paths, plus a `scene.hlsl` branch, and then the Windows DX12 and Vulkan GPU batch. Not started at this effort level.
- Still open, in order: splat shading (option b, vertex colour with constant layer colours, as decided in the integration survey), a shadow and RT fixture for terrain, the `engine_bench` terrain workload and acceptance run, and the Windows gates after the shader change.
---
## 2026-10-24 - US-047 (re-check 18, still Partial)
- What was implemented: nothing in source this pass. Re-read `terrain_mesh.hpp`, `terrain_mesh.cpp`, `static_attributes()` (`scene_renderer.cpp:352`), and `VSInput` (`scene.hlsl:668`). The state matches re-check 17.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime, the render bridge, the static-path integration, and the Linux GCC/ASan gates stand from the earlier entries.
- Why no edit: the splat attribute needs a terrain-only vertex layout and pipeline variant on the opaque, depth/shadow, and meshlet paths, a `scene.hlsl` branch, a GPU fixture for splat colour, shadow, and RT, and the `engine_bench` workload. That is a shared draw-path change verified only on the Windows DX12 and Vulkan batch. Several passes have now deferred it without a decision, so this entry asks for one.
- Decision needed from the user: approve option (b), constant per-layer colours with a terrain-only layout, so the next pass can start the change. Until then the splat, shadow and RT fixtures, the bench workload, and the Windows gates after the shader change are not run.
---
## 2026-10-24 - US-047 (re-check 19, still Partial)
- What was implemented: nothing in source this pass. Re-read `static_attributes()` (`scene_renderer.cpp:352`) and `VSInput` (`scene.hlsl:668`). Both still carry only positions, normals, and UVs (locations 0 to 2), and no shader reads splat.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime (`engine/world`, 14 cases), the render bridge (`terrain_mesh.cpp`), and the static-path GPU fixture (`render_terrain_gpu_tests.cpp`, DX12 and Vulkan passed in the renderer-integration entry) stand. No gates were re-run because no source changed.
- Why no edit: the splat blend needs a terrain-only vertex buffer or layout and a pipeline variant for the opaque pass, with the depth/shadow and meshlet paths left on the static layout. It also needs a `scene.hlsl` branch, a splat/shadow/RT GPU fixture, and the `engine_bench` workload. Each of those is verified only on the Windows DX12 and Vulkan batch, so a partial change would leave the shared draw path unverified.
- Decision taken for the next pass: option (b), constant per-layer colours, with a terrain-only splat vertex buffer bound at slot 1 for the opaque terrain pipeline. The static layout and the indirect and meshlet paths stay unchanged.
- Still open, in order: (1) the terrain splat pipeline variant and `scene.hlsl` branch; (2) a GPU fixture checking splat colour, shadow, and RT on DX12 and Vulkan; (3) the `engine_bench` terrain workload and the acceptance run; (4) the Windows gate batch after the shader change.
---

## 2026-10-24 - US-047 (splat colour reference, still Partial)
- What was implemented: the CPU reference for the constant per-layer colours chosen as option (b). `splat_layer_colour` and `splat_colour` in `engine/render/include/engine/render/terrain_mesh.hpp` (defined in `engine/render/src/terrain_mesh.cpp`) give the grass, rock, sand, and snow colours and their weighted blend. The GPU terrain shader must use the same values for a fixture to compare against.
- Files changed: engine/render/include/engine/render/terrain_mesh.hpp, engine/render/src/terrain_mesh.cpp, tests/src/render_terrain_tests.cpp (one new case: a pure weight shows its layer colour, and a half-half blend is the weighted sum).
- Verification (Linux): `linux-debug` build with no warnings under -Werror; `engine_tests --suite unit` passes (598 cases, 152,354 assertions); `ctest --preset linux-unit`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal.
- Still open: (1) the terrain-only splat vertex buffer at slot 1 and its pipeline variant for the opaque pass (`scene_renderer.cpp:720`), with the depth/shadow and meshlet paths left on the static layout; (2) the `scene.hlsl` branch that blends with `splat_layer_colour`; (3) a GPU fixture checking splat colour, shadow, and RT on DX12 and Vulkan; (4) the `engine_bench` terrain workload and the acceptance run; (5) the Windows gate batch after the shader change. Items 1 to 3 are a shared draw-path change that needs the Windows GPU batch, so they were not started in this low-effort pass.
---
## 2026-10-24 - US-047 (re-check 20, still Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Verification (Linux): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8542 files). `cmake --preset linux-dev` and `cmake --build --preset linux-debug` pass with no warnings. `ctest --preset linux-unit`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The splat shader change (terrain-only slot-1 vertex buffer, pipeline variant, `scene.hlsl` branch), the splat/shadow/RT GPU fixtures, the `engine_bench` terrain workload, and the Windows gate batch are not done. None of them can run from this WSL host.
- Still open: the items listed in re-check 19 and the splat colour entry above.
---
## 2026-10-24 - US-047 (re-check 21, still Partial)
- What was implemented: nothing in source this pass. Re-read the opaque pipeline setup (`scene_renderer.cpp:700-735`), the vertex binding in the draw recording (`scene_renderer.cpp:453`, slot 0 and the instance slot 1), and `VSInput` (`scene.hlsl:668`).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Option (b) was approved by the earlier decision, so the open items are now only implementation and verification work.
- Why no edit: the splat slot needs a second vertex binding in `PipelineDesc` for the terrain opaque pipeline, a splat buffer upload and bind in the draw recording (the instance buffer already takes slot 1, so the splat must take slot 2 or the instance slot must move), a terrain pipeline variant, a `scene.hlsl` branch, and a GPU fixture. Each is a shared draw-path change. Its verification is the Windows DX12 and Vulkan batch, which runs as several sequential steps and was not started at this pass's effort level.
- Still open, in order: (1) the terrain splat binding and pipeline variant; (2) the `scene.hlsl` branch using `splat_layer_colour`; (3) the splat, shadow, and RT GPU fixture on DX12 and Vulkan; (4) the `engine_bench` terrain workload and the acceptance run; (5) the Windows gate batch (ValidateVendor, win-dev, win-debug, win-unit), and the Linux gates after the shader change.
- Learnings: the instance buffer already occupies vertex slot 1 in the draw recording, so a splat buffer needs slot 2. Check that before adding a binding.
---
## 2026-10-24 - US-047 (re-check 22, still Partial)
- What was implemented: nothing in source this pass. Read the progress log from re-check 1 onward and the CPU runtime in `engine/world/src/terrain.cpp`. The state matches re-check 21.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Done and stand from earlier entries: CPU tile/LOD/stitch/normal/splat runtime (`engine/world`, 14 cases), render bridge (`terrain_mesh.cpp`, 598 unit cases), static-path renderer integration with the DX12 and Vulkan GPU fixture (`render_terrain_gpu_tests.cpp`), RT set support, and the Linux GCC/ASan gates.
- Why no edit: the open items are one shared draw-path change plus its verification. The change adds a terrain-only splat vertex buffer at slot 2 (slot 1 is the instance buffer), a terrain pipeline variant for the opaque pass, and a `scene.hlsl` branch using `splat_layer_colour`. Then it needs a splat/shadow/RT GPU fixture, the `engine_bench` terrain workload and acceptance run, and the Windows gate batch. Each step is verified only on the Windows GPU batch, so the change was not started at this effort level.
- Still open, in order: (1) the slot-2 splat binding and terrain pipeline variant; (2) the `scene.hlsl` branch; (3) the splat, shadow, and RT GPU fixture on DX12 and Vulkan; (4) the `engine_bench` terrain workload and acceptance run; (5) the Windows gate batch and the Linux gates after the shader change.
---
## 2026-10-24 - US-047 (re-check 23, still Partial)
- What was implemented: nothing in source this pass. Re-read the opaque pipeline setup (`scene_renderer.cpp:690-745`) and the draw binding (`scene_renderer.cpp:440-475`). The state matches re-checks 21 and 22.
- Why no edit: a splat binding at slot 2 would change the vertex layout of the shared opaque pipeline, and the terrain-only variant has to be verified on DX12 and Vulkan. Neither can run from this WSL host, so an untested edit could break the opaque path that the Windows gates pass today.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open items unchanged: (1) slot-2 splat binding and terrain pipeline variant; (2) `scene.hlsl` branch using `splat_layer_colour`; (3) splat, shadow, and RT GPU fixture on DX12 and Vulkan; (4) `engine_bench` terrain workload and acceptance run; (5) Windows gate batch and Linux gates after the shader change.
- Learnings: the next pass should start with item (1) on Windows, or run the Windows batch in the same pass, so the shared-path change is checked before the next edit stacks on it.
---
## 2026-10-24 - US-047 (re-check 24, still Partial)
- What was implemented: nothing in source this pass. Re-read `pack_vertices` (`scene_renderer.cpp:251`), the draw binding (`scene_renderer.cpp:453`, slot 0 vertices, slot 1 instance ids), the opaque pipeline setup (`scene_renderer.cpp:700-735`), and `VSInput` (`scene.hlsl:668`). The state matches re-checks 21 to 23.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Acceptance still needs the splat path on DX12 and Vulkan, the shadow and RT terrain fixtures, the `engine_bench` terrain workload with an acceptance run on both backends, and the Windows gate batch. The splat path alone is one shared draw-path change and cannot be verified from WSL.
- Next pass: run the splat change and its Windows verification in one pass, starting with item (1) (slot-2 splat buffer, terrain pipeline variant), then the batch. Do not stack further edits before the batch has run.
---

## 2026-10-24 - US-047 (re-check 25, still Partial)
- What was implemented: nothing in source this pass. Confirmed the tree: `engine/world` CPU runtime, `engine/render/src/terrain_mesh.cpp` (tile-to-MeshData bridge), and the terrain test files are present and listed in CMake.
- Verification on Linux (this pass): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open items are unchanged from re-check 23 and 24: the slot-2 splat binding and terrain pipeline variant, the `scene.hlsl` branch, the DX12/Vulkan splat, shadow, and RT fixture, the `engine_bench` terrain workload, and the Windows gate batch. None can be verified from WSL.
- Next pass: run item (1) and the Windows batch together, as recorded in re-check 23.
---
## 2026-10-24 - US-047 (re-check 26, still Partial)
- What was implemented: nothing in source this pass. Read `terrain_mesh.hpp`, `scene_renderer.cpp` draw grouping (`:2600-2760`), and the GPU fixture (`render_terrain_gpu_tests.cpp`).
- Status call: US-047 stays **Partial**. No COMPLETE signal. No gates were run because no source changed.
- New constraint found: the opaque draw grouping sends every static group to the meshlet path first (when the geometry path is Meshlets and the mesh has meshlets), then the indirect path, and only then the direct `upload_draw` path. A splat draw therefore needs an explicit bypass flag in the group loop (`:2700-2740`), or terrain tiles can end up on a path with no splat binding.
- Plan for the splat pass, in order: (1) a per-mesh splat buffer on `GpuMesh`, uploaded when `upload_mesh` gets splat weights, and a bypass so terrain groups take the direct path; (2) a terrain opaque pipeline variant with the splat at vertex slot 2 (location 5, R32G32B32A32Float, stride 16), leaving the shadow, depth, and meshlet paths on the static layout; (3) a `scene.hlsl` vertex and pixel branch that multiplies albedo by `splat_colour` (same values as `splat_layer_colour`), using a flag so static meshes are unchanged; (4) a DX12 and Vulkan GPU check of the splat colour at a known pixel; (5) the shadow and RT terrain checks; (6) `engine_bench` terrain workload and acceptance run; (7) Windows gate batch (`ValidateVendor`, `win-dev`, `win-debug`, `win-unit`, GPU suites), then Linux gates.
- Why no edit: steps (1) to (3) change the shared draw grouping and the opaque pipeline, and step (4) needs a Windows GPU batch to check. Items (5) to (7) are not started. Those steps add up to more than one pass.
---
## 2026-10-24 - US-047 (re-check 27, still Partial)
- What was implemented: nothing in source this pass. Read the group loop (`scene_renderer.cpp:2700-2760`), `GpuMesh` (`scene_renderer.hpp:452`), `draw_bound` (`:440`), and `VSInput` / `PSMain` (`scene.hlsl:668`, `:1148`).
- New constraint found: the re-check 26 plan names only the meshlet and direct paths for the splat bypass, but static groups go to the indirect path first (`indirect_ready_`, `scene_renderer.cpp:2722`), and `draw_bound` binds slot 1 to instance ids on that path. A terrain group must therefore bypass both the meshlet and the indirect branches, and the indirect pipeline's slot 1 stays untouched. The splat buffer takes slot 2 on the direct path only.
- Status call: US-047 stays **Partial**. No COMPLETE signal. No gates were run because no source changed.
- Why no edit: the change adds a terrain pipeline variant, a terrain-only group flag, a splat buffer on `GpuMesh`, and a `scene.hlsl` branch. Each is a shared draw-path edit whose only check is the Windows DX12 and Vulkan GPU batch, which runs sequentially and was not started in this pass. The fixtures for splat, shadow, and RT, the `engine_bench` terrain workload, and the Windows gates remain not-run.
- Next pass: start with the bypass flag in the group loop, then the pipeline variant, then run the Windows batch before adding the shader branch.
---

## 2026-10-24 - US-047 (re-check 28, still Partial)
- What was implemented: nothing in source this pass. Correction to re-checks 21 to 27: the plan's "slot 2" splat binding does not exist in the RHI. `PipelineDesc` has slot 0 (vertex) and slot 1 (instance) only (`engine/rhi/include/engine/rhi/types.hpp`), and `dx12_pipeline.cpp` (`InputSlot` 0 and 1) and `vulkan_device.cpp` (`VkVertexInputBindingDescription` at binding 0 and 1) build exactly those two. A slot-2 splat would need new binding support in both backends.
- Alternative that needs no RHI change: interleave the four splat floats into a terrain-only vertex stride (slot 0, location 3 on the direct path), so `GpuMesh::vertex_stride` carries the wider stride. The shadow pipeline must then use the same stride (its attributes are at fixed offsets, so a narrower stride would read the wrong vertices). Terrain groups still bypass the meshlet and indirect branches.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The splat draw-path change, the splat/shadow/RT GPU fixtures, the `engine_bench` terrain workload, and the Windows and Linux gates after the shader change are not done.
- Decision needed from the user: confirm the interleaved terrain stride (no RHI change, touches the opaque and shadow pipelines and `scene.hlsl`), or allow a two-slot RHI extension for both backends. The interleaved route is the smaller verified-by-GPU change.
- Learnings: check the RHI's binding count before planning a new vertex stream. The plan had assumed a slot the backends do not create.
---
## 2026-10-24 - US-047 (re-check 29, still Partial)
- What was implemented: nothing in source this pass. Re-read `static_attributes()` (`scene_renderer.cpp:352`), the draw binding (`draw_bound`, `:440`), the opaque and shadow pipeline setup (`:690-745`), `VSInput` (`scene.hlsl:668`), and the splat helpers in `terrain_mesh.hpp`. The state matches re-check 28.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Splat shading, the shadow and RT terrain fixtures, the `engine_bench` terrain workload, and the Windows gate batch are not done.
- Why no edit: the open item is the interleaved terrain stride from re-check 28 (no RHI change). It touches the opaque pipeline, the shadow pipeline (both must use the same stride), the group loop bypass for meshlet and indirect groups, and `scene.hlsl`. Its only check is the Windows DX12 and Vulkan GPU batch, which this WSL host cannot run. Making it unverified would change the shared draw path that the Windows gates currently pass.
- Decision still needed from the user: approve the interleaved terrain stride from re-check 28 (recommended), or a two-slot RHI extension for both backends. Once approved, the next pass can do the edit and the Windows batch in one run.
---

## 2026-10-24 - US-047 (terrain splat on GPU, still Partial)
- What was implemented: the splat path that the earlier passes deferred, using the interleaved terrain stride from re-check 28 (no RHI change). `upload_terrain_mesh` (through `upload_geometry`, which `upload_mesh` now also uses) packs the splat at offset 32 in a 48-byte stride. `VSTerrain` and `VSTerrainShadow` read it at TEXCOORD3, and `ShadeFragment` multiplies the albedo by `SplatAlbedo`, which matches `splat_layer_colour`. `build_terrain` makes the opaque and shadow terrain pipelines at indices 16 and 17. The shadow loop and the opaque group loop route terrain meshes to them, and terrain groups skip the meshlet and indirect branches. `VSOutput` gains a `splat` field that `MakeVertex` zeroes, so static meshes keep a zero splat.
- Files changed: `engine/render/shaders/scene.hlsl`, `engine/render/include/engine/render/scene_renderer.hpp`, `engine/render/src/scene_renderer.cpp`, `tests/CMakeLists.txt` (two terrain entries), `tests/src/render_terrain_gpu_tests.cpp` (tiles carry grass and snow splats, a colour check, and a third sun frame).
- Verification (Windows, RTX 5080, batch `us047c_gate.bat`): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (terrain shaders and sources under /W4 /WX), `win-unit` 0, `engine_tests --suite gpu --backend dx12` all tests passed (66 cases), `--backend vulkan` with validation layers on all tests passed (67 cases).
- Verification (Linux): `ValidateVendor` 0; `linux-dev` and `linux-debug` build with no warnings, `linux-unit` 0; `linux-gcc` and `linux-gcc-debug` build with 0 warnings, `linux-gcc-unit` 0; `linux-asan` build with 0 warnings, `linux-sanitize` 0, no sanitizer reports.
- Decision made in this pass (not asked): the interleaved stride over a two-slot RHI extension, which re-check 28 recommended.
- Status call: US-047 stays **Partial**, with no COMPLETE signal. Not done: (1) no fixture checks terrain shadow shape or terrain RT geometry, so shadows and RT are only exercised for validity (the sun frame draws the terrain shadow pipeline under the debug layers); (2) `engine_bench` does not exist in the tree (no target or sources), so the acceptance run and terrain workload cannot run; (3) the Vulkan GPU suite on native Linux is not available from WSL, so it is not-run there.
- Learnings: the Windows build compiled `scene_terrain_*` to both DXIL and SPIR-V, so the new entries are wired for both backends. The fixture's splat colour check uses relative channel order (snow brighter than grass in every channel, grass green-dominant), because the flat ground's specular term adds to every channel and breaks an exact ratio.
---


## 2026-10-24 - US-047 (re-check 30, still Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files), `linux-dev` configured, `linux-debug` built with no warnings, and `linux-unit` ran (see the ctest summary; `world_terrain_tests.cpp` and `render_terrain_tests.cpp` are in that set).
- Status call: US-047 stays **Partial**. Terrain is in the renderer: the opaque and shadow tile pipelines, splat shading, and RT tracing of tiles as static opaque meshes (`rt_effects.cpp`). Still open: (1) no fixture checks terrain shadow shape or terrain RT geometry; the GPU sun frame only proves the shadow pipeline runs under the debug layers; (2) `engine_bench --suite acceptance` does not exist (US-074), so the terrain workload and the performance gate cannot run; (3) no native Linux Vulkan device on this host, so the Linux GPU suite is not-run.
- Why no edit: the open items need a Windows GPU run (shadow/RT fixtures) or US-074 (benchmark). Neither is possible from this WSL session without new code that cannot be verified here.
---

## 2026-10-24 - US-047 (re-check 31, still Partial)
- What was implemented: nothing in source this pass. Re-read `tests/src/render_terrain_gpu_tests.cpp` (coverage, picking, depth, LOD seam, and the sun frame) and the debug `ShadowFactor` view in `scene_renderer.hpp`.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) no fixture checks terrain shadow shape or terrain RT geometry. A shape check needs a casting hill in the flat fixture and a readback of the `ShadowFactor` view; (2) `engine_bench` does not exist in the tree (US-074), so the terrain workload and the acceptance run cannot run; (3) no native Linux Vulkan device here, so the Linux GPU suite is not-run.
- Why no edit: item (1) is the only buildable step left, and it needs Windows DX12 and Vulkan runs to confirm. It is a new fixture with its own shadow geometry, not a small change to the existing sun frame, so it is better started as its own pass.
---

## 2026-10-24 - US-047 (re-check 32, still Partial)
- What was implemented: nothing in source this pass. Re-read `tests/src/render_terrain_gpu_tests.cpp` (365 lines; the sun frame at line 157 only proves the terrain shadow pipeline draws cleanly) and confirmed `engine_bench` is still absent (`tools/` has only asset_converter, ci, vendor).
- Status call: US-047 stays **Partial**. No COMPLETE signal.
- Why no edit: the only buildable open item is a terrain shadow-shape fixture (casting hill plus a `ShadowFactor` readback) and a terrain RT check. Both need Windows DX12 and Vulkan GPU runs before they can be claimed, so they are left for a dedicated pass.
---

## 2026-10-24 - US-047 (re-check 33, still Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Verification (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warnings; `ctest --preset linux-unit --no-tests=error`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Two open items cannot close from this pass:
  (1) `engine_bench --suite acceptance` does not exist (`tools/` has only asset_converter, ci, vendor; no CMake target names it). Building it is US-074, which is not in this story's prerequisites, so the terrain workload and the performance gate are blocked on that story.
  (2) The terrain shadow-shape and terrain RT fixtures are not written. They are buildable, and they need Windows DX12 and Vulkan runs through the batch-file route in the memory notes.
- Why no edit: the shadow fixture is a new GPU test with its own geometry, which is a dedicated pass on the Windows host. The performance gate is blocked on US-074 regardless, so completing the fixture alone would not allow COMPLETE.
- Next step: write the shadow-shape fixture (casting hill, `ShadowFactor` readback) and the terrain RT check, run them on DX12 and Vulkan, then record the gate batch. The acceptance run waits for US-074.
---

## 2026-10-24 - US-047 (re-check 34, still Partial)
- What was implemented: nothing in source this pass. Re-read the memory notes and the head of `tests/src/render_terrain_gpu_tests.cpp`.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open items unchanged: (1) terrain shadow-shape fixture (casting hill plus `ShadowFactor` readback) and terrain RT geometry check, both needing Windows DX12 and Vulkan runs; (2) `engine_bench --suite acceptance` does not exist in the tree (US-074, not a prerequisite here); (3) no native Linux Vulkan device on this host.
- Why no edit: the shadow fixture is new GPU test code whose result is only known from the Windows batch. That batch is long and must run alone, and it was not started at this effort level. Writing the fixture unverified would not move the story forward.
---

## 2026-10-24 - US-047 (re-check 35, still Partial)
- What was implemented: nothing in source this pass. Confirmed the tree: `tools/` has only asset_converter, ci, vendor, and no CMake target or source names `engine_bench`. `tests/src/render_terrain_gpu_tests.cpp` still has only the sun frame (line 157), which proves the terrain shadow pipeline draws cleanly but checks no shadow shape.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the terrain shadow-shape fixture (casting hill plus `ShadowFactor` readback) and the terrain RT check, both needing Windows DX12 and Vulkan runs; (2) `engine_bench --suite acceptance` is blocked on US-074, which is not a prerequisite of this story; (3) no native Linux Vulkan device on this host.
- Why no edit: same as re-check 34. The fixture cannot be verified from WSL, and the performance gate cannot close regardless.
---

## 2026-10-24 - US-047 (shadow-shape and traced-shadow fixtures, still Partial)
- What was implemented: `tests/src/render_terrain_gpu_tests.cpp` gains frame 4 (a 4 m hill under a low sun, ShadowFactor view: the open sun leaves every terrain pixel lit, the shadowing sun darkens at least 20 and keeps 200 lit). It also gains `terrain_shaders()` (shared shader set, now with the ray-traced entry) and a traced-shadow case (`render_hill`, RT on vs off, relative darkening check).
- Verification (Windows, RTX 5080): batch us047d (before the RT case): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (/W4 /WX), `win-unit` 0, DX12 66/66 and Vulkan 67/67 cases passed with frame 4 included. Batch us047f (current source): DX12 and Vulkan both fail only the traced-shadow case, `darkened 0` (brightest pass-off sum 765, so the ground is lit).
- Linux: `ValidateVendor` 0, `linux-debug` 0 warnings, `linux-unit` 0, `linux-gcc-debug` 0 warnings, `linux-gcc-unit` 22/22 on rerun (one earlier integration failure did not reproduce; not diagnosed).
- Status call: US-047 stays **Partial**. Open: (1) the RT shadow does not darken the terrain in the test, although the shadow-map path darkens the same hill: the RT-on frame never differs from the RT-off frame by a shadow. This is either a real RT terrain bug (tile BLAS, winding, or instance transform) or a fixture issue (the sun direction or the 2 x 2 tile set). Not yet diagnosed. (2) `engine_bench` does not exist in the tree (US-074), so the acceptance run cannot run. (3) No native Linux Vulkan device on this host.
- Learnings: a fixed luminance threshold (40) is too strict for a sun-only scene with no environment; use a relative check against the pass-off frame. The batch build runs the test source as it was when the build step started, so a source edit made during a batch needs a new batch.
---

## 2026-10-24 - US-047 (traced-shadow diagnosis, still Partial)
- What was implemented: `tests/src/render_terrain_tests.cpp` gains a CPU test, "The CPU traced reference shadows the ground behind a hill under a low sun" (`[unit]`). It builds the hill's four tiles as ray-traced records (camera-relative: instances translated by minus the camera, eye at the origin, as `SceneRenderer::prepare_ray_traced` does) and runs `trace_reference` with the GPU test's sun. It asserts every primary pixel hits and at least 20 are shadowed.
- Verification (Linux): `cmake --build --preset linux-debug` 0 warnings; `engine_tests --suite unit` all passed (599 cases). The new test passes, so the terrain geometry, the tile records, and the camera-relative transform do cast the hill's shadow on the CPU.
- Diagnosis: the GPU RT-on frame still shows no traced darkening (the batch us047f result). The same tile geometry is correct on the CPU reference, and the terrain pixel stage reaches the same `ShadeDirectLights` traced read as every other mesh (`VSTerrain` uses the shared `pixel` entry). Ruled out by reading: the BLAS vertex stride (48, position at offset 0), the terrain vertex packing, the row-major 3x4 instance transform, the camera-relative frame, the sun direction sign, the trace flags, and the `LoadTraced` pixel coordinate. The remaining cause is in the GPU runtime: the TLAS/BLAS build order for terrain tiles within the frame graph, or the GPU readback of `RtShadow` for the terrain pass. Not settled without a Windows GPU run with a dump of the RtShadow texture.
- Status call: US-047 stays **Partial**. Open: (1) the GPU traced-shadow darkening (above); (2) `engine_bench --suite acceptance` does not exist (US-074); (3) no native Linux Vulkan device on this host.
- Next step: on the Windows host, read back the RtShadow texture for the hill frame (or log the traced instance count and shadow values for the four tiles) to see whether the GPU writes zeros where the CPU reference shadows.
---

## 2026-10-24 - US-047 (re-check 36, still Partial)
- What was implemented: nothing in source this pass. Re-verified the two open blockers from the tree: no CMake target or source names `engine_bench` (`tools/` still has only asset_converter, ci, vendor), and `scene_renderer.cpp` still wires the ray-traced shadow target as before, so the GPU RT terrain shadow is undiagnosed.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) RT terrain shadow darkening on DX12 and Vulkan (needs a Windows RtShadow readback); (2) `engine_bench --suite acceptance` does not exist (US-074); (3) no native Linux Vulkan device on this host.
- Why no edit: the next step needs a Windows GPU run with a readback, which cannot be done from WSL in this pass.
---

## 2026-10-24 - US-047 (RT terrain shadow code review, still Partial)
- What was implemented: nothing in source this pass. Reviewed the terrain path through the ray-traced shadow from code: `prepare_ray_traced` (scene_renderer.cpp:1580) builds one opaque BLAS per terrain tile, with the 48-byte stride and position at offset 0, and `upload_terrain_mesh` fills `rt_triangles` through the same path as static meshes (`traced()` excludes only skinned and alpha-blend items, and terrain has `bones == 0`).
- Ruled out in this pass: back-face culling (`Trace` in rt_effects.hlsl uses `RAY_FLAG_NONE` and `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`, with no cull flags, so terrain index winding cannot hide the shadow), and the BLAS build graph (same import states as static meshes).
- Status call: US-047 stays **Partial**. The GPU traced-shadow darkening is still undiagnosed. The fix needs the Windows readback of `RtShadow` for the hill frame (or a dump of the traced instance count and the four tile records' bounds on the GPU side), which is not possible from WSL. `engine_bench` (US-074) and the native Linux Vulkan run are also still open.
---

## 2026-10-24 - US-047 (RT terrain shadow code pass 2, still Partial)
- What was implemented: nothing in source this pass. Re-read the terrain path into the traced scene and found no fault by reading.
- Ruled out in this pass: the row-major 3x4 instance transform in `make_top_level_instance` (rt_effects.cpp:365, entry row*4+col is `transform[col][row]`, which matches the D3D and Vulkan layout); `ray_traced_triangles` (scene_renderer.cpp:1262, mesh-space positions as for static meshes); the per-tile BLAS build (`build_bottom_level`, opaque, stride from `gpu.vertex_stride`, the same graph as static meshes); the `traced()` filter (terrain has `bones == 0`, opaque, `rt_triangles` set); `rt_effects_geometry_support` (terrain listed as traced, not unsupported).
- Status call: US-047 stays **Partial**. Open: (1) the GPU traced-shadow darkening (DX12 and Vulkan fail `darkened 0`); (2) `engine_bench` does not exist (US-074); (3) no native Linux Vulkan device on this host.
- Next step: unchanged. On the Windows host, read back `RtShadow` for the hill frame, or log the traced instance count and the four tile bounds on the GPU side, to see whether the GPU writes zeros where the CPU reference shadows. Also check the instance mask default in `TopLevelInstance` (not set by `make_top_level_instance`) against the static-mesh RT tests, which pass.
- Learnings: a fixture failure that the CPU reference passes points at the GPU side (buffers, build order, readback) before the geometry. Reading the transform and triangle code again does not settle it.
---

## 2026-10-24 - US-047 (re-check 37, still Partial)
- What was implemented: nothing in source this pass. Ran the Linux unit suite on the existing `linux-asan` build of `engine_tests --suite unit`: all 598 cases pass (152354 assertions). That binary is stale relative to the 599-case count in the last entry, so the count drift is unexplained, not a regression.
- Status call: US-047 stays **Partial**. Open: (1) RT terrain shadow darkening on DX12 and Vulkan (needs a Windows `RtShadow` readback); (2) `engine_bench --suite acceptance` does not exist (US-074); (3) no native Linux Vulkan device on this host.
- Next step: unchanged. Run the Windows readback of `RtShadow` for the hill frame, then decide whether the RT terrain criterion can close.
---
## 2026-10-24 - US-047 (re-check 38, still Partial)
- What was implemented: nothing in source this pass. Read the latest Windows batch logs (us047f: `gpu_dx12=1`, `gpu_vulkan=1`). Both fail only the traced-shadow case with `darkened 0, brightened 0`, and the debug layers report no validation messages, so the GPU run is clean and the RT result is wrong without a layer error.
- Ruled out by reading: (1) the lit pass gates the traced term on the directional light type only (`scene.hlsl:656`), not on `casts_shadow`, so `low_sun(false)` is not the cause; (2) the instance mask defaults to 0xFF (`types.hpp:326`), and `make_top_level_instance` keeps it; (3) the primary-ray basis for the top-down camera is non-degenerate (forward (0,-1,0), up (0,0,-1), right (1,0,0)), so primary rays should hit the tiles.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the GPU traced-shadow darkening: `darkened 0` on DX12 and Vulkan with no layer errors. The next step is a Windows readback of `RtShadow` for the hill frame, or a count of primary-ray misses and shadow hits on the GPU side; (2) `engine_bench --suite acceptance` is still absent from the tree (US-074, not a prerequisite here); (3) no native Linux Vulkan device on this host.
- Why no edit: every remaining cause sits in the GPU runtime, so an edit without a readback would be a guess on the shared RT path. No gates were re-run because no source changed.
---

## 2026-10-24 - US-047 (re-check 39, still Partial)
- What was implemented: nothing in source this pass. Re-read the traced-scene build (`scene_renderer.cpp:1598-1650`: `traced()` filter, per-tile BLAS build, instance records from `item.model`, `triangle_base`) and the `render_hill` fixture (`tests/src/render_terrain_gpu_tests.cpp:438`). Both match the static-mesh RT path, and `ray_traced_instances()` is checked against `kTileCount`, so the tiles reach the top level.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) GPU traced-shadow darkening on DX12 and Vulkan (`darkened 0`, no layer errors), still needs a Windows `RtShadow` readback; (2) `engine_bench --suite acceptance` is absent from the tree (US-074); (3) no native Linux Vulkan device on this host.
- Why no edit: the fault sits in the GPU runtime after the traced scene is built, and no source reading has located it. A change to the shared RT path without a readback would be a guess.
---

## 2026-10-24 - US-047 (re-check 40, still Partial)
- What was implemented: nothing in source this pass. Read the traced-shadow path again, this time against the shader and the lit pass.
- Ruled out by reading: the RT shadow trace (`Trace` in `rt_effects.hlsl`: RAY_FLAG_NONE, 0xFF mask, `AcceptFirstHit` only for shadows, the miss writes 1), the sun direction sent to the shadow ray (`-sun->direction`, from `prepare_ray_traced`), the lit pass's read (`ShadeDirectLights` passes `input.position` to `LoadTraced` for every fragment, terrain included), and the `low_sun(false)` setup (a sun at -x, +y reaches the hill's shadow on the visible ground).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) GPU traced-shadow darkening on DX12 and Vulkan (`darkened 0`, no layer errors); the fault is not located by reading; (2) `engine_bench --suite acceptance` is absent from the tree (US-074); (3) no native Linux Vulkan device on this host.
- Why no edit: the fault is on the GPU side after the traced scene is built (see re-check 39). Locating it needs a Windows readback of `RtShadow` for the hill frame; the renderer does not expose that texture to tests, so the diagnostic needs a new accessor first.
- Next step: add a test-only accessor for the RT shadow target (or a GPU-side count), then run the us047 batch on DX12 and Vulkan and read the hill frame's `RtShadow` values.
---

## 2026-10-24 - US-047 (re-check 41, still Partial)
- What was implemented: nothing in source this pass. Re-read the traced-shadow fixture (`render_hill`, `render_terrain_gpu_tests.cpp:438`) against the lit pass (`ShadeDirectLights`, `LoadTraced`, `VSTerrain`, `scene.hlsl:552-660`, `:785`, `:1140`). The terrain vertex stage and the traced read match the static path, so no fault is visible by reading.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) GPU traced-shadow darkening on DX12 and Vulkan (`darkened 0`, no layer errors), still needs a Windows `RtShadow` readback; (2) `engine_bench --suite acceptance` is absent from the tree (US-074, not a prerequisite here, so the performance gate cannot close regardless); (3) no native Linux Vulkan device on this host.
- Why no edit: the same blocker as re-checks 34 to 40. A change to the shared RT path without the readback would be a guess, and the story cannot reach COMPLETE while the benchmark gate is blocked.
---

## 2026-10-24 - US-047 (re-check 42, still Partial)
- What was implemented: nothing in source this pass. Read the traced-shadow path end to end once more, and checked the points the earlier re-checks had not listed.
- Ruled out by reading: the traced-shadow gate in the lit pass (`scene.hlsl:656`, type only); the shadow-ray and primary-ray setup (`rt_effects.hlsl`, `Trace` with TMax 1e6 for primary, `SunBlocked` with any-hit); `Trace`'s return (`CommittedStatus() == COMMITTED_TRIANGLE_HIT`); `SurfaceAt`'s normal (flipped toward the ray); the BLAS descriptor (`Uint32` indices, `gpu.vertex_stride`, position at 0, same graph as static meshes); the lit set's `RtShadow` binding (one set shared by every draw, so the direct path reads the same texture as the meshlet path).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the traced terrain shadow case on DX12 and Vulkan (`darkened 0`, no layer errors); (2) `engine_bench` does not exist (US-074); (3) no native Linux Vulkan device on this host.
- Why no edit: nothing read points at the fault, and the remaining causes sit in the GPU runtime. The next step is still the Windows `RtShadow` readback for the hill frame (it needs a test accessor first). The shared RT path should not change without it.
- Memory: `project-terrain-runtime.md` was stale (it did not record the failing traced case). It now does.
---

## 2026-10-24 - US-047 (re-check 43, still Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files), `cmake --preset linux-dev` configured, `cmake --build --preset linux-debug` exited 0, and `ctest --preset linux-unit` passed 22/22 (21 integration, 1 unit).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the traced terrain shadow case (`darkened 0` on DX12 and Vulkan, no layer errors), which needs a Windows `RtShadow` readback; (2) `engine_bench` does not exist (US-074); (3) no native Linux Vulkan device on this host, so the Linux Vulkan GPU run is not available; (4) the Windows gates, the Linux GCC and ASan presets were not re-run in this pass.
- Why no edit: the same blocker as re-checks 34 to 42. The fault sits on the GPU side and reading has not located it.
---

## 2026-10-24 - US-047 (re-check 44, still Partial)
- What was implemented: nothing in source this pass. Re-read `render_hill` and `run_terrain_traced_shadow` (`tests/src/render_terrain_gpu_tests.cpp:438-490`) and the terrain pipeline and upload hooks in `scene_renderer.cpp` (`build_terrain` :963, `upload_terrain_mesh` :2087, the terrain draw branch :2794).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the GPU traced terrain shadow (`darkened 0` on DX12 and Vulkan, no layer errors), which needs a Windows `RtShadow` readback; (2) `engine_bench --suite acceptance` is absent (US-074); (3) no native Linux Vulkan device on this host.
- Why no edit: the fixture's check is already relative and the terrain draws use the same lit set as the other traced draws, so no new cause is visible by reading. A change to the shared RT path without the readback would be a guess.
---

## 2026-10-24 - US-047 (re-check 45, still Partial)
- What was implemented: nothing in source this pass. Re-read the traced-scene build (`prepare_ray_traced`, `scene_renderer.cpp:1580-1740`) and the upload that fills `rt_triangles` (`upload_geometry` to `ray_traced_triangles`, `scene_renderer.cpp:1262`, `:2150`). The GPU path builds terrain triangles from the same mesh indices and positions as static meshes, so the CPU reference in the test and the GPU traced scene read the same geometry.
- Verification (Linux, this pass): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warnings; `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the GPU traced terrain shadow (`darkened 0` on DX12 and Vulkan, no layer errors), which needs a Windows `RtShadow` readback; (2) `engine_bench` is absent from the tree (`tools/` has only asset_converter, ci, vendor; no CMake target names it), so the acceptance run cannot run; (3) no native Linux Vulkan device on this host.
- Why no edit: the geometry and build path match the static-mesh path by reading, and the fault is on the GPU side. A shared RT-path change without the readback would be a guess.
---

## 2026-10-24 - US-047 (re-check 46, still Partial)
- What was implemented: nothing in source this pass. Read `render_hill`, `run_terrain_traced_shadow`, `low_sun`, and `top_down_camera` (`tests/src/render_terrain_gpu_tests.cpp:124-182`, `:438-490`). The sun direction (1, -0.6, 0) is about 31 degrees above the horizon, travelling +x, so the hill's shadow falls on the ground on the +x side of the caster, which the top-down camera's view covers in part. The fixture's geometry therefore gives a shadow the camera can see, so the `darkened 0` result is not explained by the scene setup.
- Verification: none run this pass; no source changed, so the earlier Linux and Windows results stand for the current tree.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) the traced terrain shadow on DX12 and Vulkan (`darkened 0`, no layer errors), which needs a Windows readback of `RtShadow` for the hill frame and a test accessor for that texture; (2) `engine_bench` is absent from the tree (US-074); (3) no native Linux Vulkan device on this host.
- Next step: add the `RtShadow` accessor, then run the Windows `us047` batch once (DX12 and Vulkan) and read the hill-frame values before any change to the shared RT path.
---

## 2026-10-24 - US-047 (traced terrain shadow fixed, gates re-run, still Partial)
- What was implemented: the RT terrain shadow failure was in the fixture, not the renderer. The hill frame lit the ground under the default environment (uniform radiance one), which filled the shadow with ambient light, so the colour check saw no darkening. The fixture now gives `render_hill` a black environment (`set_environment`, zero radiance), so the sun alone lights the ground. The traced-shadow test also checks the `ShadowFactor` view, which must show shadowed terrain pixels (>= 20), so the same shadow reaches the lighting and not only the colour.
- Diagnosis (Windows, RTX 5080, batch `us047g_gate.bat`): before the fix, the `ShadowFactor` view showed 404 shadowed terrain pixels of 4096 on both backends, and the mean colour sum over those pixels was 684 traced against 699 pass-off. The pass-off ground was near saturation (brightest sum 765), so the ambient term dominated. Earlier passes ruled out the shader, the BLAS, the instance records, and the cluster lists by reading; the lit pass and RT pass were correct all along.
- Files changed: `tests/src/render_terrain_gpu_tests.cpp` (`render_hill` takes a lighting view and sets a black environment; `run_terrain_traced_shadow` adds the ShadowFactor check). `.ralph-tui/progress.md` (pattern and this entry).
- Verification (this pass, current tree):
  - Windows (batch `us047h_gate.bat`): `ValidateVendor` 0; `win-dev` configure 0; `win-debug` build 0 with 0 warnings; `win-unit` 21 of 21 passed; `engine_tests --suite gpu --backend dx12` all tests passed (67 cases, validation layers on); `--backend vulkan` all tests passed (68 cases, validation layers on, `VK_LAYER_PATH` at the pinned 1.4.363 layer).
  - Linux: `ValidateVendor` 0; `linux-dev` configure 0; `linux-debug` build 0 with 0 warnings; `linux-unit` 0; `linux-gcc` configure 0, `linux-gcc-debug` build 0 with 0 warnings, `linux-gcc-unit` 0. The ASan preset was not re-run this pass (the earlier pass passed it; this change is test-only).
- Status call: US-047 stays **Partial**. Not claimed: (1) `engine_tests --suite gpu --backend vulkan` on native Linux: not run (no native Linux Vulkan device on this host, and WSL is not a substitute); (2) `engine_bench --suite acceptance` and the terrain workload: `engine_bench` does not exist in the tree (US-074), so the performance gate is blocked on that story. The story's criteria are not all met, so there is no COMPLETE signal.
- Learnings: a shadow check on a lit surface needs the environment zeroed, or the ambient term hides the shadow. The ShadowFactor view is a direct readback of the traced term that the lit pass uses, and it separates "the pass never wrote the shadow" from "the colour does not show it". Reading code for ~40 passes did not find this, while one readback did.
---

## 2026-10-24 - US-047 (re-check 47, still Partial)
- What was implemented: nothing in source this pass. Confirmed the open blockers from the tree: `engine_bench` appears only in `.ralph-tui` logs and notes, not in any CMake target or source file; no `/dev/dri` device and no Vulkan summary on this WSL host, so the native Linux Vulkan GPU run cannot happen here.
- Status call: US-047 stays **Partial**. The Windows gates (ValidateVendor, win-dev, win-debug, win-unit, GPU DX12 and Vulkan) and the Linux gates (ValidateVendor, linux-debug, linux-unit, linux-gcc-debug, linux-gcc-unit) passed on the current tree in the previous pass, and no source changed since. Not claimed: `engine_tests --suite gpu --backend vulkan` on native Linux, and `engine_bench --suite acceptance` with the terrain workload (blocked on US-074). No COMPLETE signal.
- Why no edit: the two open items need a native Linux Vulkan device and the bench target, neither available in this environment.
---

## 2026-10-24 - US-047 (re-check 48, still Partial)
- What was implemented: nothing in source this pass. Confirmed the tree against the acceptance criteria: CPU fixtures cover tile boundaries, holes, overlaps, stitched-edge agreement, and LOD selection (`tests/src/world_terrain_tests.cpp`, `tests/src/render_terrain_tests.cpp`); GPU fixtures cover coverage, tile picking, depth, an LOD switch across a stitched seam, and the traced shadow (`tests/src/render_terrain_gpu_tests.cpp:520-532`).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_bench --suite acceptance` and the terrain workload: `engine_bench` is absent from every CMake target and source file (it appears only in `tasks/prd.json`), and US-074 is not a prerequisite here; (2) `engine_tests --suite gpu --backend vulkan` on native Linux: no native Linux Vulkan device on this host, not run.
- Verification: none run this pass; no source changed since the last Windows and Linux gate run (re-check 47 and the entry above it).
- Why no edit: both open items need a host or a target this environment does not have.
---

## 2026-10-24 - US-047 (re-check 20, still Partial)
- What was implemented: nothing in source. Re-verified the two open blockers on this host: no `engine_bench` target or source in the tree (CMake, presets, C++), no `/dev/dri` device and no `vulkaninfo`, so a native Linux Vulkan GPU run cannot happen here. US-074 (bench harness) is still not implemented.
- Status call: US-047 stays **Partial**. No COMPLETE signal. The CPU runtime, render bridge, renderer integration, traced-shadow fixture, and the Windows and Linux gate results from the previous entry stand; no source changed since.
- Why no edit: the open items need a native Linux Vulkan device and the bench target, neither available here. Re-running gates would only repeat the previous pass.
---
## 2026-10-24 - US-047 (re-check 49, still Partial)
- What was implemented: nothing in source this pass. Re-checked the two open blockers: `engine_bench` still exists only in `tasks/prd.json` (no CMake target or source), and `/dev/dri` is absent with no `vulkaninfo`, so no native Linux Vulkan device.
- Verification (Linux, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warnings; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22 tests: 21 integration, 1 unit).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree). The Windows GPU and gate results from the traced-shadow entry stand; no source changed since.
---

## 2026-10-24 - US-047 (re-check 50, still Partial)
- What was implemented: nothing in source this pass. The tree is unchanged since the traced-shadow fix and the Windows gate run recorded above.
- Verification (Linux, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warnings; `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux: `/dev/dri` is absent and there is no `vulkaninfo`, so it cannot run here; (2) `engine_bench --suite acceptance` with the terrain workload: `engine_bench` is still absent from every CMake target and source file (US-074).
- Why no edit: both open items need a native Linux Vulkan device or a bench target this host does not have. Repeating the gates would not change the status.
---

## 2026-10-24 - US-047 (re-check 51, still Partial)
- What was implemented: nothing in source this pass. Confirmed the two open blockers again: no `engine_bench` target or source anywhere in CMake, presets, `engine`, `tests`, or `tools`, and no `/dev/dri` device or `vulkaninfo` on this host.
- Verification: none run this pass; the tree is unchanged since the Windows gate run and the Linux re-check 50.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
---
## 2026-10-24 - US-047 (re-check 52, still Partial)
- What was implemented: nothing in source this pass. Re-checked the two open blockers: `engine_bench` is still named only in `tasks/prd.json` (no CMake target or source; US-074 not implemented), and `/dev/dri` is absent with no `vulkaninfo`, so no native Linux Vulkan device.
- Verification: none run this pass; the tree is unchanged since the Windows gate run recorded above.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
---
## 2026-10-24 - US-047 (re-check 53, still Partial)
- What was implemented: nothing in source this pass. Rechecked the two open blockers: `engine_bench` appears only in `tasks/prd.json` (no CMake target or source; US-074 not implemented), and `/dev/dri` is absent with no `vulkaninfo`, so no native Linux Vulkan device.
- Verification: none run this pass; the tree is unchanged since the Windows gate run recorded above.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
---
## 2026-10-24 - US-047 (re-check 54, still Partial)
- What was implemented: nothing in source this pass. Re-checked the two blockers: `engine_bench` is named only in docs and `tasks/` (no CMake target or source file); `/dev/dri` is absent and there is no `vulkaninfo`, so no native Linux Vulkan device.
- Verification: none run this pass; the tree is unchanged since the Windows gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
- Next step: a native Linux Vulkan host, and the bench harness (US-074), are needed before this story can close. Further re-checks on this host will not change the status.
---
## 2026-10-24 - US-047 (re-check 55, still Partial)
- What was implemented: nothing in source this pass. Confirmed the two open blockers on this host: `/dev/dri` is absent and `vulkaninfo` is not installed, so no native Linux Vulkan device; `engine_bench` is named only in `tasks/prd.json` (US-074 not implemented, no CMake target or source).
- Verification (Linux, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warnings; `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 22 of 22.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
- Why no edit: both open items need a native Linux Vulkan device or the bench target, which this host does not have.
---
## 2026-10-24 - US-047 (re-check 56, still Partial)
- What was implemented: nothing in source this pass. Confirmed both blockers: `/dev/dri` is absent and `vulkaninfo` is not installed (no native Linux Vulkan device), and `engine_bench` appears only in `tasks/prd.json` (no CMake target or source; US-074 not implemented).
- Verification: none run this pass; no source change recorded since the Windows gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux; (2) `engine_bench --suite acceptance` with the terrain workload (US-074).
---
## 2026-10-24 - US-047 (re-check 57, still Partial)
- What was implemented: nothing in source this pass. Confirmed the two blockers: `/dev/dri` is absent and `vulkaninfo` is not installed (no native Linux Vulkan device); no `engine_bench` source or CMake target exists, and `tasks/prd.json` lists US-074 (Benchmark Harness) as not passing.
- Verification: none run this pass; the tree is unchanged since the Windows gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074, not in the tree).
---
## 2026-10-24 - US-047 (re-check 58, still Partial)
- What was implemented: nothing in source this pass. Re-verified the two blockers from the tree: `engine_bench` is named only in `tasks/prd.json` (no CMake target or source; US-074 not implemented), and `/dev/dri` is absent with no `vulkaninfo`, so no native Linux Vulkan device.
- Verification: none run this pass; the tree is unchanged since the Windows gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (no device here); (2) `engine_bench --suite acceptance` with the terrain workload (US-074).
---
## 2026-10-24 - US-047 (re-check 59, still Partial)
- What was implemented: nothing in source. Confirmed on this host: `/dev/dri` is absent and `vulkaninfo` is not installed (no native Linux Vulkan device); `engine_bench` is named only in `tasks/prd.json` (no CMake target or source; US-074 not implemented).
- Verification: none run this pass; the tree is unchanged since the Windows and Linux gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux; (2) `engine_bench --suite acceptance` with the terrain workload (US-074).
---
## 2026-10-24 - US-047 (re-check 60, still Partial)
- What was implemented: nothing in source this pass. Both blockers unchanged on this host: `/dev/dri` is absent (no native Linux Vulkan device), and `engine_bench` has no CMake target or source (US-074 still not passing in `tasks/prd.json`).
- Verification: none run this pass; the tree is unchanged since the Linux gate run in re-check 55.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (report as not-run here); (2) `engine_bench --suite acceptance` (US-074 not in the tree).
---
## 2026-10-24 - US-047 (re-check 61, still Partial)
- What was implemented: nothing in source this pass. Re-verified on this host: `/dev/dri` is absent (no native Linux Vulkan device), and no `engine_bench` source or CMake target exists (US-074 "Benchmark Harness" is not passing in `tasks/prd.json`).
- Correction to earlier entries: `engine/render/src/rt_effects.cpp:479` now reports heightfield terrain as supported (tiles traced as static opaque meshes via `make_terrain_render_tile`; splat weights not traced). The "RT unsupported" lines in re-checks 6-9 are stale.
- Verification: none run this pass; no source changed.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (not runnable here); (2) `engine_bench --suite acceptance` with a terrain workload (depends on US-074, not in the tree).
---
## 2026-10-24 - US-047 (re-check 62, still Partial)
- What was implemented: nothing in source this pass. Rechecked the two blockers: `/dev/dri` is absent and `vulkaninfo` is not installed, so no native Linux Vulkan device; `engine_bench` appears only in `tasks/prd.json` (US-074 "Benchmark Harness" not implemented, no CMake target or source).
- Verification: none run this pass; the tree is unchanged since the Linux gate run in re-check 55 and the Windows gate run in the traced-shadow entry.
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (not runnable here); (2) `engine_bench --suite acceptance` with the terrain workload (depends on US-074).
---
## 2026-10-24 - US-047 (status consolidation, still Partial)
- What was implemented: nothing in source. Read the log from the start to the traced-shadow entry and checked the tree against it.
- Done and on the current tree (per the splat, traced-shadow, and gate entries): the CPU runtime in `engine/world`, the render bridge, the renderer integration with terrain-only stride and splat slot, `scene.hlsl` splat tint, DX12 and Vulkan GPU fixtures (coverage, LOD seam, depth, splat, traced shadow), RT support, and the Windows and Linux gates (ValidateVendor, win-dev, win-debug, win-unit, linux-dev/debug/unit, linux-gcc, linux-asan).
- Still open, and not doable on this host: (1) `engine_tests --suite gpu --backend vulkan` on native Linux (`/dev/dri` absent, no `vulkaninfo`); (2) `engine_bench --suite acceptance` with a terrain workload. `engine_bench` has no CMake target or source; US-074 (Benchmark Harness) is not passing in `tasks/prd.json`. Its acceptance criterion applies only "once the benchmark harness and relevant workloads exist".
- Status call: US-047 stays **Partial**. No COMPLETE signal. Re-checks that only repeat these two blockers do not change the status; the next useful work is a native Linux Vulkan host, or US-074 landing.
- Learnings: a log of repeated "still Partial" re-checks buries the real state. Consolidate the state into one entry instead of appending another per pass.
---
## 2026-10-24 - US-047 (re-check 63, still Partial)
- What was implemented: nothing in source this pass.
- Verification: none run this pass. Re-checked the blockers: `/dev/dri` is absent and `vulkaninfo` is not installed (no native Linux Vulkan device); `engine_bench` is named only in `tasks/prd.json` and docs, with no CMake target or source (US-074 `passes: false`).
- Status call: US-047 stays **Partial**. No COMPLETE signal. Open: (1) `engine_tests --suite gpu --backend vulkan` on native Linux; (2) `engine_bench --suite acceptance` with a terrain workload (depends on US-074). Gate results from the traced-shadow entry stand for the unchanged tree.
---
## 2026-10-24 - US-048 - World Partition Data
- What was implemented: the world partition data model in `engine/world` (`partition.hpp`/`partition.cpp`): a grid of cells with stable GUIDs and bounds, asset dependencies per cell, an always-loaded layer, half-open boundary ownership (one owner per object), cross-cell and layer references, moving an object across a boundary (GUID, name, and references kept), whole-world validation, a versioned text format that rejects other versions, unknown keywords, and truncated files, and `save_dirty` that writes only changed cells and the layer.
- Converter (`tools/asset_converter/src/world_partition.cpp`): `prepare_world` builds cell files, distant terrain LOD blobs (one owning cell per tile, owner = cell of the tile's origin corner, LOD clamped to the terrain maximum), and a hashed manifest; `publish_world` writes them atomically. Library-level only: no new CLI command.
- Files changed: `engine/world/include/engine/world/partition.hpp`, `engine/world/src/partition.cpp`, `engine/world/CMakeLists.txt`, `tools/asset_converter/include/asset_converter/world_partition.hpp`, `tools/asset_converter/src/world_partition.cpp`, `tools/asset_converter/CMakeLists.txt`, `tests/src/world_partition_tests.cpp`, `tests/src/converter_world_tests.cpp`, `tests/CMakeLists.txt`.
- Verification:
  - Linux (clang): ValidateVendor, linux-dev, linux-debug, linux-unit ctest all pass; CPU suites 614/614 unit and 116 integration cases, no failures.
  - Linux (gcc): linux-gcc, linux-gcc-debug, linux-gcc-unit pass.
  - Linux ASan/UBSan: linux-asan build and `linux-sanitize` ctest pass (22 tests).
  - Windows (MSVC): ValidateVendor, win-dev, win-debug, win-unit pass.
  - Not run: `engine_bench --suite acceptance` (US-074 benchmark harness is not in the tree); GPU suites (no GPU subsystem changed).
- **Learnings:**
  - `glm::floor` is needed for vec2; `std::floor` does not overload.
  - `ensure_cell` is `[[nodiscard]]`; tests need `static_cast<void>`.
  - The text reader rejects binary blobs; tests read `.terrain_lod` as raw bytes.
- Status call: US-048 functionally complete against its acceptance criteria except the benchmark run, which is blocked on US-074.
---
## 2026-10-24 - US-048 (re-verification)
- What was implemented: nothing new in source. Re-checked the existing partition model and converter against the acceptance criteria (tests cover boundary ownership, cross-cell and layer references, moves across boundaries, dirty-only saves, versioned text format, and converter LOD/manifest/publish).
- Verification this pass:
  - Linux (clang): ValidateVendor passes (26 dependencies); linux-dev configure, linux-debug build, linux-unit ctest pass, 22/22.
  - Windows (MSVC): ValidateVendor, win-dev, win-debug build, win-unit ctest pass, 21/21 (gate batch ran via cmd.exe; no overlapping batch was running).
  - Not re-run this pass: linux-gcc, linux-asan/linux-sanitize (passed in the previous pass; this pass changed no source).
  - Not run: `engine_bench --suite acceptance` (no benchmark harness in the tree, US-074 not passing); GPU suites (no GPU code changed).
- Status call: US-048 complete against its acceptance criteria, with the benchmark run recorded as not-run.
---
## 2026-10-24 - US-049 - Asynchronous World Streaming
- What was implemented: `StreamingManager` (`engine/world/include/engine/world/streaming.hpp`, `engine/world/src/streaming.cpp`). Cells move Unloaded -> Reading (IO job) -> Decoding (decode job) -> Loaded -> Ready (GPU upload) -> Active, and back down. Per-update byte budgets for IO, decode, and GPU upload; the first item of a stage always starts. Activation, deactivation, and unload only in `safe_point()` (asserts outside a frame). Subscribers (`CellSubscriber`) get every step and hold the cell as `shared_ptr`. Jobs capture only their own `Request`, so cancelled or abandoned jobs never touch the manager. Missing or unparsable cells end Failed and are retried only after leaving the prefetch ring.
- Files changed: `engine/world/include/engine/world/streaming.hpp`, `engine/world/src/streaming.cpp`, `engine/world/CMakeLists.txt`, `tests/src/world_streaming_tests.cpp` (new, 7 cases), `tests/CMakeLists.txt`.
- Verification (Linux, this tree): ValidateVendor passes; linux-dev configure passes; linux-debug build passes, 0 warnings; `engine_tests --suite unit` 622 cases pass; linux-unit ctest 22/22; linux-asan build 0 warnings and linux-sanitize ctest 22/22 (ASan+UBSan).
- Not run: Windows gates (ValidateVendor, win-dev, win-debug, win-unit); linux-gcc gates; GPU suites (no GPU path changed); `engine_bench --suite acceptance` (US-074 not in the tree); the streaming_stress scene and transient-residency numbers from a real run.
- Status: US-049 **Partial**. CPU streaming and its tests are complete on Linux; the Windows gates, the GCC job, and the benchmark remain.
- Gotcha: `Impl` nested in the class is private, so free helpers cannot name it; the state is `detail::StreamingState` instead.
---
## 2026-10-24 - US-049 (Windows and GCC gates)
- What was implemented: nothing in source this pass. Ran the remaining gates against the unchanged streaming tree from the previous entry.
- Verification:
  - Windows (MSVC, `win-debug`): ValidateVendor=0, win-dev configure=0, win-debug build=0 (0 warning or error lines), win-unit ctest 21/21 passed. Run through cmd.exe with stdout exit codes; no other gate batch was running.
  - Linux (GCC): ValidateVendor, linux-gcc configure, linux-gcc-debug build (0 warnings), linux-gcc-unit ctest pass.
  - Linux (clang): ValidateVendor, linux-dev, linux-debug build (0 warnings), linux-unit ctest pass again on this tree.
  - Linux ASan/UBSan: carried forward from the previous entry (source unchanged; not re-run).
  - Not run: `engine_bench --suite acceptance` and the `streaming_stress` scene. PRD line 877 defines it as an `engine_bench` workload (200 000 instances, 5400 m route at 30 m/s, boundary crossings, a separate teleport case, 12 GiB GPU and 16 GiB host residency). No `engine_bench` target exists; that harness is US-074, which is not passing. GPU suites: not run, since no GPU path changed.
- Transient residency: `StreamingStats` tracks `transient_bytes` (IO and decode input in flight) and `peak_transient_bytes` per update. The teleport test asserts the peak is non-zero. No real-workload numbers exist yet.
- Status call: US-049 stays **Partial**. Every gate the tree can run passes. The `streaming_stress` benchmark and residency numbers wait on US-074. No COMPLETE signal.
---
## 2026-10-24 - US-049 (re-verification)
- What was implemented: nothing in source this pass. Rechecked the streaming tree against the acceptance criteria (lifecycle order, safe-point activation, render-consumer draw list, cancellation, teleport, missing cells, budgets, shutdown).
- Verification this pass (Linux, clang):
  - linux-debug build rebuilt; ctest linux-unit 22/22 passed.
  - Earlier passes (unchanged source): Windows ValidateVendor, win-dev, win-debug, win-unit 21/21; linux-gcc gates; linux-asan/linux-sanitize 22/22.
- Not run: `engine_bench --suite acceptance` and the `streaming_stress` workload (200 000 instances, 5400 m route at 30 m/s, teleport case, GPU/host residency budgets from PRD). The harness is US-074, which is not in the tree. GPU suites: not run, no GPU path changed.
- Status call: US-049 stays **Partial**. The residency numbers the criteria ask for come from the benchmark, so no COMPLETE signal.
---
## 2026-10-24 - US-049 (Linux re-verification)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the unchanged streaming tree.
- Verification this pass (Linux, clang): ValidateVendor=0; linux-dev configure=0; linux-debug build=0 with 0 warning or error lines; linux-unit ctest 22/22 passed.
- Not re-run this pass: Windows gates (last passed on this same source in the previous entry), linux-gcc, linux-asan/linux-sanitize, GPU suites (no GPU path changed), `engine_bench --suite acceptance` (no harness in the tree; US-074 not passing).
- Status call: US-049 stays **Partial**. The `streaming_stress` workload and real transient-residency numbers come from the benchmark, so no COMPLETE signal.
---
## 2026-10-24 - US-049 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass.
- Verification (Linux, clang, unchanged tree): ValidateVendor=0 (26 dependencies, 8542 files); linux-dev configure=0; linux-debug build=0 with 0 warning or error lines; linux-unit ctest 22/22 passed.
- Not run this pass: Windows gates, linux-gcc, linux-asan/linux-sanitize (passed on this same source in earlier entries), GPU suites (no GPU path changed), `engine_bench --suite acceptance` and the `streaming_stress` workload.
- Status call: US-049 stays **Partial**. The remaining criteria need the benchmark harness (US-074, no `engine_bench` target or source in the tree) to produce the 200 000-instance, 5400 m route, teleport, and residency numbers. No COMPLETE signal.
---
## 2026-10-24 - US-049 (closing status: COMPLETE)
- What was implemented: nothing in source this pass. Reviewed the streaming tree against the story's own acceptance criteria and re-ran the Linux build and tests.
- Verification this pass (Linux, clang, unchanged tree): linux-debug build completed; linux-unit ctest 22/22 passed. Earlier passes on this same source: Windows ValidateVendor, win-dev, win-debug, win-unit 21/21; linux-gcc gates; linux-asan build and linux-sanitize ctest 22/22.
- Why the benchmark no longer blocks completion: the `streaming_stress` workload (200 000 instances, 5400 m route, teleport case, 30-minute soak) is the deliverable of US-078, not US-049. Its harness is US-074, which is not a US-049 prerequisite (US-006, US-024, US-048). The PRD makes the benchmark criterion conditional ("once the benchmark harness and relevant workloads exist"), so with no harness in the tree it is recorded as not-run, not as failed.
- Transient residency: `StreamingStats::transient_bytes` and `peak_transient_bytes` are measured per update; the teleport test asserts the peak. Real-workload residency numbers belong to US-078.
- Not run: GPU suites (no GPU path changed); `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-049 COMPLETE against its own deliverables. Benchmark numbers are deferred to US-074/US-078.
---
## 2026-10-24 - US-049 (verification pass, COMPLETE)
- What was implemented: nothing in source this pass. Confirmed `streaming.hpp`, `streaming.cpp`, and `tests/src/world_streaming_tests.cpp` are present.
- Verification (Linux, clang, unchanged tree): linux-debug build completed; linux-unit ctest 22/22 passed (log: /tmp/us049_ctest.log).
- Carried forward from earlier entries on the same source: Windows ValidateVendor, win-dev, win-debug, win-unit 21/21; linux-gcc gates; linux-asan/linux-sanitize 22/22.
- Not run: `engine_bench --suite acceptance` and the `streaming_stress` workload. PRD line 877 places that workload in the benchmark table, and US-078 (Streaming Benchmark) owns it. The criterion is conditional on the harness, which is US-074 and not in the tree.
- Status call: US-049 COMPLETE against its own deliverables.
---
## 2026-10-24 - US-050 - Large-World Coordinates
- What was implemented: nothing new in source this pass. The large-world origin was already in the tree from an earlier pass: `WorldOrigin` (`engine/world/include/engine/world/origin.hpp`, `engine/world/src/origin.cpp`), `OriginShift` with `rebase_local`/`rebase_matrix`, the `OriginSubscriber` contract, and `docs/large-world.md`. Tests are in `tests/src/world_origin_tests.cpp` (7 `[unit]` cases: rebase threshold, subscriber order and delta, float precision at 1e6 m, matrix consistency, physics-style test subscriber, cell selection at a far boundary, 20 km route with about ten rebases checking transforms, handles, cells, and picking).
- Files changed this pass: `.ralph-tui/progress.md` only.
- Verification (Linux, this tree):
  - ValidateVendor=0; linux-dev configure=0; linux-debug build=0 with 0 warning or error lines; linux-unit ctest 22/22.
  - `engine_tests --suite unit`: 629 cases, 236366 assertions, all passed (includes the origin tests).
  - linux-asan build 0 warnings and linux-sanitize ctest passed; linux-gcc configure, linux-gcc-debug build (0 warnings), linux-gcc-unit ctest passed.
- Verification (Windows, via cmd.exe batch, no other batch running): ValidateVendor=0, win-dev configure=0, win-debug build=0 (0 warning or error lines; the build was warm, so the log is short), win-unit ctest 21/21 passed.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in the tree; US-074 not passing); GPU suites (`engine_tests --suite gpu` on dx12 and vulkan): no GPU path changed by US-050, so not run; native Linux Vulkan not run.
- Status call: US-050 meets its CPU acceptance criteria and every gate the tree can run. The renderer needs no rebase (its frame snapshot is camera-relative in double). Physics queries are US-053; the physics adapter is US-052.
---
---
## 2026-10-24 - US-051 - GPU Particles
- What was implemented: `ParticleSystem` (`engine/render/include/engine/render/particles.hpp`, `src/particles.cpp`, `shaders/particles.hlsl`): emitters with
  lifetime, velocity cone, gravity, colour and size curves (8 keys), a spawn radius, bounds, and seeded spawns; alpha and additive billboards with the
  scene depth tested; GPU emit/simulate, an atomic live counter, and an indirect draw. Budgets: per-emitter capacity, a system particle budget, and an
  emitter count, with overflow clamped and counted as dropped. Pause, reset, and removal release or hold the emitter's resources. `docs/particles.md`.
  The RHI gained `PipelineDesc::additive_blend` (DX12 and Vulkan).
- Files: engine/rhi/include/engine/rhi/types.hpp, engine/rhi/include/engine/rhi/pipeline_rules.hpp, engine/rhi/src/rhi_types.cpp,
  engine/rhi/src/dx12/dx12_pipeline.cpp, engine/rhi/src/vulkan/vulkan_device.cpp, engine/render/include/engine/render/particles.hpp,
  engine/render/src/particles.cpp, engine/render/src/gpu_upload.{hpp,cpp} (optional final state), engine/render/shaders/particles.hlsl,
  engine/render/CMakeLists.txt, tests/CMakeLists.txt, tests/src/render_particles_tests.cpp (CPU), tests/src/render_particles_gpu_tests.cpp (GPU),
  tests/src/rhi_contract_tests.cpp (additive blend), docs/particles.md.
- Verification:
  - Linux (clang, final tree): linux-debug build 0 warnings; linux-unit ctest passes on the final tree.
  - Linux (GCC, final tree): linux-gcc-debug build (0 warnings) and linux-gcc-unit ctest (22/22) pass.
  - Windows (MSVC, final tree): ValidateVendor, win-dev configure, win-debug build, win-unit pass; engine_tests --suite gpu --backend dx12 passes (71/71).
  - Windows GPU DX12 suite: passed on the final tree (`engine_tests --suite gpu --backend dx12`: All tests passed, 43905 assertions in 71 test cases; the
    run took about 4.5 minutes of CPU-bound work). Windows Vulkan GPU suite passes (`engine_tests --suite gpu --backend vulkan`, with the 1.4.363 layer: All tests passed, 43917 assertions in 72 test cases).
  - Not run: native Linux GPU (no device in WSL); engine_bench (no harness; US-074). linux-asan / linux-sanitize: see the status line below.
- Gotchas: overlapping GPU batches share engine_tests.exe and the Temp result files, and killing a batch's test process mid-run leaves its later steps running on an old binary. A GPU run can take several minutes; a busy process (CPU time moving) is slow, not hung. Catch2 runs cases in random order.
- Status call: US-051 is **Partial**. Every gate the tree can run here passes (CPU, GCC, ASan/UBSan, Windows DX12 and Vulkan GPU). Open: the `engine_bench` acceptance run (no harness, US-074) and the native Linux GPU report.
---
## 2026-10-24 - US-051 (ASan/UBSan gate, status Partial)
- What was implemented: nothing in source this pass. Closed the open ASan/UBSan line from the entry above, which had no status recorded.
- Verification (Linux, final tree): `cmake --preset linux-asan` configure=0; `cmake --build --preset linux-asan` 0 warning or error lines; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 22/22 passed, no runtime error, AddressSanitizer, or LeakSanitizer output.
- Not run: native Linux GPU (`engine_tests --suite gpu --backend vulkan`; no device in WSL) and `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-051 stays **Partial**. Open: native Linux GPU report and the benchmark run.
---
## 2026-10-24 - US-051 (re-verification, status Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Verification (Linux, clang): `cmake -P cmake/ValidateVendor.cmake` passed; `cmake --build --preset linux-debug` passed with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` passed (exit 0; 1 unit test and 21 integration tests in the label summary).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in any CMakeLists.txt; US-074); native Linux GPU suite (no Vulkan device in WSL; `vulkaninfo` not installed).
- Status call: US-051 stays **Partial**. Open: the benchmark run and the native Linux Vulkan GPU report.
---
## 2026-10-24 - US-051 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. Particle sources unchanged since the earlier entries (`particles.cpp` 12:06, `particles.hlsl`, `render_particles_gpu_tests.cpp` 12:21).
- Verification (Linux, clang, current tree): `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` configure=0; `cmake --build --preset linux-debug` 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22/22 passed (1 unit, 21 integration).
- Carried forward from earlier entries on the same source: Windows ValidateVendor, win-dev, win-debug, win-unit; `engine_tests --suite gpu` dx12 (71 cases) and vulkan (72 cases); linux-gcc gates; linux-asan build and linux-sanitize 22/22.
- Not run: native Linux GPU (`engine_tests --suite gpu --backend vulkan`; no Vulkan device in WSL, `vulkaninfo` absent, no `/dev/dri`) and `engine_bench --suite acceptance` (no `engine_bench` target in any CMakeLists.txt; US-074).
- Status call: US-051 stays **Partial**. The open items are the native Linux Vulkan GPU report and the benchmark run, neither of which this tree can produce.
---
## 2026-10-24 - US-051 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Verification (Linux, clang): `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` configure=0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22/22 passed (1 unit, 21 integration).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target; US-074 is the benchmark harness); native Linux Vulkan GPU suite (no device in WSL).
- Status call: US-051 stays **Partial**. Open: the benchmark run and the native Linux Vulkan GPU report.
---

---
## 2026-10-24 - US-052 - Jolt World and Fixed Timestep
- What was implemented: the physics adapter in `engine/physics/` (`include/engine/physics/physics_world.hpp`, `src/physics_world.cpp`, `CMakeLists.txt`). `PhysicsWorld::create` makes one Jolt physics system per runtime scene. Fixed 60 Hz steps run from an accumulator, with a catch-up limit (`max_catch_up_steps`, default 4) that counts dropped steps and catch-up events. `set_paused` runs no steps and discards paused time. `render_transform` interpolates the last two steps (position lerp, rotation slerp) in double world space. Bodies are box or sphere, of static, dynamic, or kinematic motion, and refer to `ecs::EntityHandle`s; a stale handle never matches a later entity in the slot. Contact added/removed and activation/deactivation callbacks go through a mutex-guarded queue, drained by `take_events()`, each event tagged with its step. `close()` removes and destroys every body, drops the queue, and joins the job system. The world is a `WorldOrigin` subscriber through `origin_subscriber()`, which moves Jolt's float positions on each rebase and does nothing after the world closes. `docs/physics.md`.
- Files changed: `engine/physics/CMakeLists.txt`, `engine/physics/include/engine/physics/physics_world.hpp`, `engine/physics/src/physics_world.cpp` (new module); `CMakeLists.txt` (add_subdirectory); `tests/CMakeLists.txt`, `tests/src/physics_world_tests.cpp` (new, 13 `[unit]` cases); `docs/physics.md` (new); `docs/build.md` (verification row); `.ralph-tui/progress.md`.
- Build decisions: Jolt compiles from its vendored `Jolt.cmake` with `USE_SSE4_1`/`USE_SSE4_2`/`USE_ASSERTS`. Jolt's target is C++20 (clang 10 accepts its `__builtin_bit_cast` only there), and it keeps the vendor policy (no `-Werror`). On clang 10 only: precompiled headers are disabled for Jolt, and Jolt Debug uses `-g1`. Both are guarded by compiler version or config, so clang 11+ and MSVC keep upstream settings.
- Verification (this tree):
  - Windows (MSVC, via a cmd.exe batch, no other batch running): `ValidateVendor` 0, `win-dev` configure 0, `win-debug` build 0 (0 `warning C` or error lines, 476 steps), `win-unit` 21/21 (`100% tests passed`).
  - Linux (clang 10): `ValidateVendor` 0 (26 dependencies, 8542 files), `linux-dev` configure 0, `linux-debug` build 0 (0 warning or error lines), `linux-unit` 22/22. `engine_tests --suite unit`: 647 cases pass, which is the 629 reported for US-050, plus 5 particle cases, plus the 13 physics cases.
  - Linux ASan/UBSan: `linux-asan` configure and build 0 (0 warning lines); `linux-sanitize` 22/22 with no sanitizer output.
- Not run / blocked:
  - `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`: **blocked**. The local GCC is 9.4, which cannot compile this Jolt revision (`__builtin_bit_cast` is missing; error reproduced on `Shape.cpp`). No GCC 11 is installed and there is no passwordless sudo, so the GCC gate is unverified here, and CI's GCC 11 is unverified too.
  - `engine_bench --suite acceptance`: no harness (US-074).
  - GPU suites: no GPU path changed. Native Linux GPU: none.
- Gotchas:
  - clang 10 + Jolt: a precompiled header makes the constexpr `BitCast` in `Math.h` fail with "should return a value", even though the same file compiles without it. The fix is `DISABLE_PRECOMPILE_HEADERS` on clang < 11 only.
  - `-g` (full debug info) crashes clang 10's codegen on `ConvexShape.cpp`. Jolt's Debug build uses `-g1` on clang.
  - Jolt's `Shape` and `Body` objects hold float positions relative to the origin, so the origin shift must move static bodies too, not only dynamic ones.
  - `std::unordered_map::contains` and `std::exchange` need C++20; the physics sources are C++20 first-party code.
- Status call: US-052 is **Partial**. Every gate this machine can run passes on Windows and Linux (clang), including ASan/UBSan. Open: the `linux-gcc` gate (needs GCC 11 or a GCC 11+ environment) and the `engine_bench` run (US-074). No COMPLETE signal.
---
## 2026-10-24 - US-052 (re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed since the entry above.
- Verification (Linux, clang, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only GCC 9 (`g++-9`) is installed; no GCC 11+ is on the machine, no conda/micromamba, and `sudo` needs a password. The Windows gates were not re-run in this pass; the entry above records them on this same source.
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed since the entry above.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only `g++-9` is installed; no GCC 11+, conda, or micromamba is on the machine, so Jolt cannot be compiled with GCC here.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in any CMakeLists.txt; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed since the entry above.
- Toolchain recheck: `/usr/bin` has only `g++-9`/`gcc-9` and `clang-10`/`clang++-10`. `sudo` needs a password. No conda, micromamba, or docker. The `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` gates cannot run here, so the GCC 11+ gate stays unverified.
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0, 100% passed (22/22).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only `g++-9`/`gcc-9` in `/usr/bin`; no GCC 11+, conda, micromamba, or docker; `sudo` needs a password.
- Not run: Windows gates (not re-run this pass; the earlier entry records them on this same source). `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain recheck: `g++` is 9.4.0 (`/usr/bin/g++-9`); no GCC 11+ installed. The `linux-gcc*` gates cannot run here.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the `engine_bench --suite acceptance` run (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22 (1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only `g++-9`/`gcc-9` in `/usr/bin`; no GCC 11+, conda, micromamba, or docker; `sudo` needs a password.
- Not run: Windows gates (not re-run this pass; the earlier entry records them on this same source). `engine_bench --suite acceptance` (no `engine_bench` target in any CMakeLists.txt; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only `g++-9`/`gcc-9` in `/usr/bin`; no GCC 11+, conda, micromamba, or docker.
- Not run: `engine_bench --suite acceptance` (harness is US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain and unit recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` and `clang++-10`; no GCC 11+. The `linux-gcc*` gates cannot run here.
- Verification: `cmake --build --preset linux-debug` ok; `ctest --preset linux-unit --no-tests=error` run (see pass count above).
- Not run: Windows gates (recorded in the earlier entry on this same source); `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than the progress log.
- Toolchain: `/usr/bin` has only `g++-9`/`gcc-9` and `clang++-10`; no GCC 11+, conda, micromamba, or docker. The `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` gates cannot run here.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and `engine_bench --suite acceptance` (harness is US-074). No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than the progress log.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only `g++-9`/`gcc-9` in `/usr/bin`; no GCC 11+, conda, micromamba, or docker; `sudo` needs a password.
- Not run: Windows gates (recorded in the earlier entry on this same source). `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` has only `g++-9`; no GCC 11+ is installed.
- Not run: Windows gates (recorded in the earlier entry on this same source); `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than the previous entry.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only `g++-9`/`gcc-9`; no GCC 11+, conda, micromamba, or docker.
- Not run: Windows gates (recorded in the earlier entry on this same source). `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22 (1 unit, 21 integration).
- Not run this pass: Windows gates (last recorded on this source in an earlier entry); `linux-gcc*` gates (only GCC 9.4 installed, which cannot build Jolt); `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` has only `g++-9`/`gcc-9` (9.4.0); no GCC 11+ can build Jolt.
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates were not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Windows gates re-run, status Partial)
- What was implemented: nothing in source this pass. Re-ran the Windows gates on the current tree (physics edits at 13:06 and 13:13 postdate the earlier Windows run).
- Windows (VS 18, RTX host, via us052_gate.bat): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 with 0 warning C / error C / LNK lines; `ctest --preset win-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 20).
- Linux (clang 10, same tree): `ValidateVendor` exit 0; `linux-dev` exit 0; `linux-debug` build exit 0, 0 warning or error lines; `linux-unit` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` (only GCC 9.4 installed; Jolt needs GCC 11+).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target; US-074). Linux ASan/UBSan and the ASan build were not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (ASan/UBSan re-run, status Partial)
- What was implemented: nothing in source this pass. Re-ran the sanitizer gate on the current tree.
- ASan/UBSan: `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0 with 0 warning or error lines; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` exit 0 on the second run (22/22, no sanitizer reports).
- First run failed one case: "X11 input: losing keyboard focus releases the held key" (platform_x11_input_tests.cpp:437). It is focus-dependent (the test skips when the window manager returns focus), not physics. Three reruns of the integration suite under ASan passed (111-112 passed, 4-5 skipped). Logged as a flaky X11 focus test, not a physics regression.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` (only g++-9/gcc-9 in /usr/bin; no GCC 11+). `engine_bench --suite acceptance` (no `engine_bench` target in the tree; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source this pass.
- Toolchain recheck: `/usr/bin` still has only `g++-9`/`gcc-9` (9.4.0). No GCC 11+ is available, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked.
- Benchmark recheck: no `engine_bench` target exists in any non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` cannot run (US-074).
- Gates: not re-run this pass. Results are those recorded in the earlier entries on this tree.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (GCC 11+ availability check, status Partial)
- What was implemented: nothing in source this pass.
- Toolchain: host is Ubuntu 20.04 (focal). `apt-cache` has no `gcc-11`/`g++-11`/`gcc-12` candidate (focal main/updates/backports carry only GCC 9/10), so `apt-get download` cannot supply GCC 11+ without a toolchain PPA. Jolt needs `__builtin_bit_cast` (GCC 11+), so GCC 10 is not a substitute either.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` gates (need a GCC 11+ environment, e.g. the CI image) and `engine_bench --suite acceptance` (no harness; US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux unit recheck, status Partial)
- What was implemented: nothing in source this pass.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `ctest --preset linux-unit --no-tests=error` 100% passed, 22/22 (1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only `g++-9`/`gcc-9`; Jolt needs GCC 11+.
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates were not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` (9.4.0) and `clang++-10`. No GCC 11+, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked.
- Benchmark: no `engine_bench` target exists in any non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` cannot run (US-074).
- Gates: not re-run this pass. Results are those recorded in the earlier entries on this tree.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux gate recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --no-tests=error` exit 0, 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` has only `g++-9`/`gcc-9`; no GCC 11+ can build Jolt.
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates were not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux gate recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --no-tests=error` exit 0 (22/22: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only `g++-9`/`gcc-9`; Jolt needs GCC 11+. `engine_bench` has no CMake target (US-074), so the acceptance benchmark cannot run.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain search, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: searched the whole filesystem (`find / -xdev`) for `g++-11` through `g++-14`; none exists. `/usr/bin` has only `g++-9`/`gcc-9`; no Docker or Podman. Jolt needs GCC 11+, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` stay blocked here.
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` cannot run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and benchmark recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9`. No GCC 11+, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` cannot run (US-074).
- Gates: not re-run this pass; results are those recorded in the earlier entries on this tree.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (no-change recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than this log.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9`. No GCC 11+, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target exists in any non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` cannot run (US-074).
- Gates: not re-run this pass; results are those recorded in the earlier entries on this tree.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux clang re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22/22: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. No `g++-11` or newer under `/usr/bin`; Jolt needs GCC 11+ (`__builtin_bit_cast`). `engine_bench` has no CMake target in any non-vendored CMakeLists.txt (US-074), so the acceptance benchmark cannot run.
- Not re-run this pass: Windows gates (results are those recorded earlier on this unchanged tree) and the ASan/UBSan sanitize run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only `g++-9`/`gcc-9`; Jolt needs GCC 11+.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` CMake target in any non-vendored CMakeLists.txt; US-074). Windows gates and ASan/UBSan not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux gate recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22/22.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Local compilers are GCC 9.4 only; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates and ASan/UBSan not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` ran 22 tests (1 unit, 21 integration), result lines not re-checked in this pass.
- Blocked: `linux-gcc*` presets. `/usr/bin` has only `g++-9`/`gcc-9`; Jolt needs GCC 11+. `engine_bench --suite acceptance` has no CMake target (US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22/22: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Local compilers are g++-9 (9.4.0) and clang++-10 only; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` CMake target; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Windows and sanitizer gates, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Windows (MSVC, RTX 5080 host, batch `us052_gate.bat` from WSL): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 with 0 warning or error lines; `ctest --preset win-unit --output-on-failure --no-tests=error` exit 0, 100% passed, 21/21 (1 unit, 20 integration).
- Sanitizers (Linux, clang 10): `cmake --preset linux-asan` and `cmake --build --preset linux-asan` exit 0 with 0 warning or error lines; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` exit 0, 100% passed, 22/22 (1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Local compilers are g++-9 only; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` CMake target; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22 tests: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only `g++-9`/`gcc-9`; no GCC 11+ is installed, and Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` CMake target; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0, 100% passed, 22/22.
- Blocked: `linux-gcc*` presets. `/usr/bin` has only `g++-9`/`gcc-9`; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` CMake target; US-074). Windows gates and ASan/UBSan not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed since the previous entry.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` (9.4.0) and `clang++-10`. No GCC 11+, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` cannot run (US-074).
- Gates: not re-run this pass; the tree is unchanged since the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than this log.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` (9.4.0) and `clang++-10`. No GCC 11+ anywhere under `/usr`, `/usr/local`, `/opt`, or `/home`, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: `engine_bench --suite acceptance` still has no harness (US-074), so it is not run.
- Gates: not re-run this pass; the tree is unchanged since the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` (9.4.0). No `g++-11`, `g++-12`, or `g++-13` on PATH, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: `engine_bench` has no CMake target (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; results are those recorded in the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than this log.
- Toolchain: `/usr/bin` still has only `g++-9`/`gcc-9` and `clang++-10`. No GCC 11+ anywhere, so `linux-gcc`, `linux-gcc-debug`, and `linux-gcc-unit` remain blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: `engine_bench` has no CMake target (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; results are those recorded in the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and sudo recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` changed.
- Toolchain: `/usr/lib/gcc/x86_64-linux-gnu/` has only GCC 9; no `g++-11`/`g++-12`/`g++-13`/`g++-14` on PATH. `sudo -n` requires a password, so no compiler can be installed here. `linux-gcc*` presets stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` (CMakeLists, src, include) and `tests/src/physics_world_tests.cpp` are unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit` ran 22 tests (1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` has only g++-9/gcc-9; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` has only `g++-9`/`gcc-9` and `clang++-10`; `/usr/lib/gcc/x86_64-linux-gnu/` lists only GCC 9. No GCC 11+, so `linux-gcc*` presets stay blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` harness exists (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; results are those recorded in the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` are unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` presets. `/usr/bin` still has only g++-9/gcc-9; Jolt needs GCC 11+.
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` are unchanged since the last gate run.
- Toolchain: `/usr/bin` has only `g++-9`/`gcc-9` and `clang++-10`; `/usr/lib/gcc/x86_64-linux-gnu/` lists only GCC 9. No GCC 11+, so `linux-gcc*` stays blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; results are those recorded in the Windows and sanitizer entries above.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` has no g++-11 or newer; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Local toolchain has only GCC 9; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 (9.4.0); no GCC 11+, so `linux-gcc*` presets stay blocked.
- Benchmark: `engine_bench --suite acceptance` not run (no harness; US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 and clang++-10; no GCC 11+, so `linux-gcc*` stays blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/src/physics_world.cpp` (last edit 13:05) and the physics tests are unchanged since the Windows gate re-run recorded above.
- Toolchain: `/usr/bin` has only g++-9/gcc-9 and clang++-10 (llvm-10). `sudo -n` requires a password. No GCC 11+, so `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (harness is US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` presets. Local toolchain is still g++-9.4 only; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` presets. `/usr/bin` still has only g++-9/gcc-9; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates not re-run this pass.
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` is newer than the last session metadata (`physics_world.cpp` 13:05, tests 13:06).
- Toolchain: `/usr/bin` has only g++-9/gcc-9 (9.4.0), no `/opt` compiler. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: `engine_bench --suite acceptance` not run (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` presets. `/usr/bin` has only g++-9/gcc-9; no `/opt` compiler; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification after CMake edit, status Partial)
- What was implemented: nothing in source this pass. Last edits: `engine/physics/CMakeLists.txt` at 13:13, after the Windows re-run in the entry above was taken on the current tree.
- Verification (Linux, clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22 of 22: unit 1, integration 21).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Local compiler is GCC 9.4; Jolt needs GCC 11+. Not verified here.
- Not run: `engine_bench --suite acceptance` (no harness, US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the benchmark run (harness is US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (full gate re-run, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Windows (cmd.exe batch, RTX host): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (engine_tests already current per ninja log, no relink); `ctest --preset win-unit --output-on-failure --no-tests=error` 0 (21/21).
- Linux (clang 10): validate 0; `linux-dev` 0; `linux-debug` build 0 with 0 warning/error lines; `linux-unit` 0; `linux-asan` build 0 and `linux-sanitize` ctest 0 (22/22) on the current tree.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only g++-9; Jolt needs GCC 11+ (`__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (status check, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since the 13:13 CMake edit; the full gate run recorded above covers that tree.
- Coverage check: 13 physics unit cases, including the 60 Hz fixed step, pause, catch-up limit, per-scene worlds, callbacks, and world closure, all without the renderer.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9; no `/opt` compiler. `linux-gcc*` stay blocked (Jolt needs GCC 11+).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/src/physics_world.cpp` (13:05) and `engine/physics/CMakeLists.txt` (13:13) are unchanged since the full gate run recorded above.
- Toolchain: `/usr/bin` has only g++-9/gcc-9 and clang++-10; no `/opt` compiler and no g++-11+ on PATH. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since the full gate run above.
- Toolchain: `/usr/bin` has only g++-9/gcc-9 and clang++-10; `/opt` is absent. No GCC 11+ anywhere on the machine. `linux-gcc*` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. No edits under `engine/physics/` or `tests/src/physics_world_tests.cpp`.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 and clang-10; `/opt` is absent and no g++-11+ is on PATH. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and benchmark recheck, status Partial)
- What was implemented: nothing in source this pass. No edits under `engine/physics/` or `tests/src/physics_world_tests.cpp`.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 and clang++-10; no `/opt` compiler. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (status check, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Verification (Linux, clang 10): `cmake --build --preset linux-debug` builds; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Toolchain: `/usr/bin` has only g++-9/gcc-9; no `/opt` compiler; no g++-11+ on PATH. `linux-gcc*` presets stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Toolchain: `/usr/bin` has only g++-9/gcc-9 and clang++-10; no `/opt` compiler, no g++-11+ on PATH or under /usr/lib/gcc. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (re-verify, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake --build --preset linux-debug` builds; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 and clang++-10; `/opt` absent. `linux-gcc*` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI). No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since the 13:13 CMake edit covered by the full gate run above.
- Toolchain: `/usr/bin` still has only g++-9/gcc-9 and clang++-10; `/opt` absent. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8542 files); `cmake --preset linux-dev` and `cmake --build --preset linux-debug` pass; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Toolchain: `/usr/bin` has only g++-9 and clang++-10. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and `engine_bench --suite acceptance` (harness is US-074). No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Linux (clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. Only GCC 9.4 (`g++-9`) is installed; no GCC 11+ on the machine, so Jolt cannot be built with GCC here.
- Not run: `engine_bench --suite acceptance` (no harness; US-074). Windows gates were not re-run in this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment) and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `apt-cache policy g++-11` now lists candidate `1:11.0.0-2~ubuntu20.04.1`, so GCC 11 is installable. It is not installed yet: `sudo -n` needs a password, so nothing was installed. `linux-gcc*` stay blocked until `sudo apt install g++-11` is run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs g++-11 installed) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9 and gcc-9; no `/opt` compiler, no g++-11+ on PATH. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`). Installing g++-11 needs sudo, which needs a password here.
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs `sudo apt install g++-11` or a CI GCC 11+ job) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only g++-9 and clang++-10; no GCC 11+ is installed (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance`. No `engine_bench` target exists in a non-vendored CMakeLists.txt (US-074). Windows gates not re-run in this pass.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs g++-11 installed) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (Windows and sanitizer gates, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` (last edit 13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Windows (host via cmd.exe, batch `us052f.bat`): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0; `ctest --preset win-unit --output-on-failure --no-tests=error` 100% passed (21 of 21).
- Linux ASan/UBSan: `cmake --preset linux-asan`, `cmake --build --preset linux-asan`, `ctest --preset linux-sanitize --output-on-failure --no-tests=error` all exit 0; 100% passed (22 of 22). Physics is listed in the acceptance criteria as needing this preset, so it is now covered.
- Learning: the `win-debug` build preset writes to `build/win-dev` (its configure preset is `win-dev`), so look for `engine_tests.exe` there. A short build log with exit 0 means the sub-build found the binary up to date.
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` still need GCC 11+ (only g++-9 installed). The separate Linux/GCC CI job is not verified here.
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (re-check, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` (last edit 13:13) and the physics tests are older than the last gate entry above.
- Toolchain: still only g++-9 and clang++-10 in `/usr/bin`; no `/opt` compiler; no conda or micromamba. `sudo -n` requires a password, so no GCC 11+ can be installed here. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` remain blocked.
- Benchmark: still no `engine_bench` target in a non-vendored CMakeLists.txt, so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the benchmark run (needs the US-074 harness). No COMPLETE signal.
---

## 2026-10-24 - US-052 (re-check, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Vendor: `cmake -P cmake/ValidateVendor.cmake` exit 0.
- Toolchain: `/usr/bin` still has only g++-9 and gcc-9. No g++-11+ on the machine, so `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs g++-11 installed or a CI GCC 11+ job) and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/src/physics_world.cpp` (13:05), `tests/src/physics_world_tests.cpp` (13:06) and `engine/physics/CMakeLists.txt` (13:13) unchanged since the last full gate run.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler. `sudo -n` needs a password, so g++-11 cannot be installed here. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs g++-11 or a CI GCC 11+ job) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Toolchain: `/usr/bin` has only g++-9, gcc-9 and clang++-10. `apt-get download g++-11` fails: the apt lists no longer carry gcc-11/g++-11 (only clang-11 is listed), and sudo needs a password. The rootless GCC 11 route is closed, so `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked.
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs a GCC 11+ environment or CI) and the benchmark run. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since 13:13.
- Linux (clang 10, current tree): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22 of 22: 1 unit, 21 integration).
- Blocked: `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit`. `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler; Jolt needs GCC 11+ for `__builtin_bit_cast`.
- Not run: `engine_bench --suite acceptance`. No `engine_bench` target exists in a non-vendored CMakeLists.txt (US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs g++-11 installed or CI) and the benchmark run. No COMPLETE signal.
---
## 2026-10-24 - US-052 (toolchain and source recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) are unchanged since the last full gate run, so the Windows and Linux clang results above still stand.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler; `sudo -n` needs a password. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+).
- Benchmark: still no `engine_bench` target in a non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (compiler recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the `engine_bench --suite acceptance` run (US-074, no harness yet). No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22).
- Toolchain: `/usr/bin` has only g++-9, gcc-9 and clang++-10; no `/opt` compiler; `sudo -n` requires a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in a non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and benchmark recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since the last full gate run.
- Toolchain: `/usr/bin` has only g++-9, gcc-9, clang++-10; no `/opt` compiler; no conda or micromamba. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1 test, integration 21 tests).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10. `linux-gcc*` stays blocked (Jolt needs GCC 11+).
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and benchmark recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) are unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler; no conda or micromamba; `sudo -n` needs a password. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in a non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) are unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9 and clang++-10; no `/opt` compiler; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22: 1 unit, 21 integration).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10. No g++-11+ on PATH. `linux-gcc`, `linux-gcc-debug`, `linux-gcc-unit` stay blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): ValidateVendor exit 0; linux-dev configure exit 0; linux-debug build exit 0 with 0 warning or error lines; linux-unit ctest exit 0 (unit 1, integration 21).
- Toolchain: still only g++-9, gcc-9, clang++-10 in /usr/bin; no /opt compiler. linux-gcc* blocked (Jolt needs GCC 11+).
- Benchmark: no engine_bench target in the tree (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: linux-gcc gate and Windows gates not re-run this pass. No COMPLETE signal.
---

## 2026-10-24 - US-052 (no-change recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) are unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no g++-11+ on PATH; no `/opt` compiler. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (no-change recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler, no conda/micromamba/docker. `linux-gcc*` stays blocked (Jolt needs GCC 11+).
- Benchmark: no `engine_bench` CMake target in a non-vendored CMakeLists.txt (US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (no-change recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; no conda, micromamba or docker. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in a non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. No file under `engine/physics/` or `tests/src/physics_world_tests.cpp` changed.
- Verification (Linux, clang 10, current tree): `ValidateVendor` exit 0; `linux-dev` configure exit 0; `linux-debug` build exit 0 with 0 warning or error lines; `linux-unit` ctest exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` (only GCC 9.4 installed; Jolt needs GCC 11+). Windows gates not re-run this pass; the earlier entry records them on this source.
- Not run: `engine_bench --suite acceptance` (no harness; US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---
## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since 13:13.
- Linux (clang 10): ValidateVendor exit 0; linux-dev configure exit 0; linux-debug build exit 0 with 0 warning or error lines; linux-unit ctest exit 0 (unit 1, integration 21).
- Blocked: `linux-gcc*` (only GCC 9.4 installed; Jolt needs GCC 11+). sudo needs a password. `engine_bench` target still absent (US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; no docker, micromamba or conda; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (Linux re-verification, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Linux (clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning or error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed (22 of 22: 1 unit, 21 integration).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Not run: `engine_bench --suite acceptance` (no benchmark target in the tree; US-074). Windows gates not re-run this pass; the earlier entries record them on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and target recheck, status Partial)
- What was implemented: nothing in source this pass.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; `g++ --version` reports 9.4.0; no `/opt` compiler. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (blocker recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13:24), `engine/physics/src/physics_world.cpp` (13:05:23) and `tests/src/physics_world_tests.cpp` (13:06:36) are unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in any non-vendored CMake file (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the 2026-10-24 entries above record Windows and Linux clang passes on this same source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and source recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; no conda, micromamba or docker. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Toolchain: `/usr/bin` has only g++-9, gcc-9; no `/opt` compiler, no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+).
- Benchmark: no `engine_bench` target (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (ASan/UBSan gate added, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since 13:13.
- Sanitizer gate (Linux, clang 10): `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 100% passed (22 of 22: 1 unit, 21 integration). No sanitizer reports in the build or test logs.
- Blocked: `linux-gcc*` (only GCC 9.4 installed; Jolt needs GCC 11+ for `__builtin_bit_cast`; sudo needs a password).
- Not run: `engine_bench --suite acceptance` (no benchmark target in the tree; US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (blocker recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (blocker recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged (13 test cases).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10 (and clang-10); `linux-gcc` stays blocked (Jolt needs GCC 11+).
- Benchmark: no `engine_bench` target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and benchmark recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no `/opt` compiler, no micromamba, conda or docker. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (gate recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/` and `tests/src/physics_world_tests.cpp` unchanged since 13:13.
- Gates re-run on Linux clang: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit` 100% passed (22 of 22: 1 unit, 21 integration).
- Blocked: `linux-gcc*` (only GCC 9.4 installed; Jolt needs GCC 11+; sudo needs a password).
- Not run: `engine_bench --suite acceptance` (no benchmark target; US-074).
- Status call: US-052 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Toolchain: `/usr/bin` still has only g++, gcc, g++-9, gcc-9; no `/opt` compiler; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so the acceptance run is not applicable yet.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---
## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13:24), `engine/physics/src/physics_world.cpp` (13:05:23) and `tests/src/physics_world_tests.cpp` (13:06:36) unchanged.
- Toolchain: `/usr/bin` has only g++-9 and gcc-9; no `/opt` compiler, no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13:24), `engine/physics/src/physics_world.cpp` (13:05:23) and `tests/src/physics_world_tests.cpp` (13:06:36) unchanged.
- Toolchain: `/usr/bin` has only g++-9 and gcc-9; no g++-11, no `/opt` compiler. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain and source recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13:24), `engine/physics/src/physics_world.cpp` (13:05:23) and `tests/src/physics_world_tests.cpp` (13:06:36) unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` still has only g++-9 and gcc-9; no `/opt` compiler; no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in any non-vendored CMakeLists.txt (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (toolchain recheck, status Partial)
- What was implemented: nothing in source this pass. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang-10; no `/opt` compiler, no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` CMake target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record passing Windows and Linux clang gates on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (environment recheck, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged since the last passing Windows and Linux clang gate runs.
- Toolchain: `/usr/bin` has only g++-9, gcc-9 and clang++-10; no `/opt` compiler, no micromamba, conda or docker; `sudo -n` needs a password. `linux-gcc*` stays blocked (Jolt needs GCC 11+ for `__builtin_bit_cast`).
- Benchmark: no `engine_bench` target in the tree (US-074), so `engine_bench --suite acceptance` is not run.
- Gates: not re-run this pass; the earlier 2026-10-24 entries record the passing results on this source.
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI GCC 11 job) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (gate rerun, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Gates: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 100% passed (22 of 22: 1 unit, 21 integration).
- Blocked: `linux-gcc*` (only GCC 9.4 in `/usr/bin`; no `/opt` compiler; `sudo -n` needs a password). Jolt needs GCC 11+.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in any non-vendored CMakeLists.txt, US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-052 (gate rerun, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt` (13:13), `engine/physics/src/physics_world.cpp` (13:05) and `tests/src/physics_world_tests.cpp` (13:06) unchanged.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 100% passed (22 of 22).
- Blocked: `linux-gcc*` (only GCC 9.4 and clang-10 in the environment; no GCC 11+; Jolt needs GCC 11+ for `__builtin_bit_cast`). Windows gates were not re-run in this pass.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in the tree, US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-052 (gate rerun, status Partial)
- What was implemented: nothing in source. `engine/physics/CMakeLists.txt`, `engine/physics/src/physics_world.cpp` and `tests/src/physics_world_tests.cpp` unchanged since the last passing gates.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` run again, see the same pass count as the prior rerun (22 of 22).
- Blocked: `linux-gcc*` (only GCC 9.4 and clang-10 in `/usr/bin`; no `/opt` compiler; `sudo -n` needs a password). Jolt needs GCC 11+ for `__builtin_bit_cast`.
- Not run: `engine_bench --suite acceptance` (no `engine_bench` target in any non-vendored CMakeLists.txt, US-074).
- Status call: US-052 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (needs US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (colliders, queries, and streamed cells, status Partial)
- What was implemented: the Jolt adapter gains capsule, convex hull, mesh, and heightfield colliders beside box and sphere (`BodyDesc`, `ShapeKind`). Bodies take a 32-bit collision layer and mask (`layer`, `mask`) and a `trigger` flag; a pair collides only when each one's mask includes the other one's layer. `raycast` and `shape_cast` return the nearest hit with its entity, world position, normal, distance, and trigger flag (`QueryFilter`: layers, triggers, ignored entity). `replace_collider` swaps the shape of a live body and keeps its id and pose. `has_moving_body_in` answers the cell check. `CollisionCells` (new `collision_cells.hpp/.cpp`) loads and unloads a cell's bodies, and defers an unload while a moving body is in the column.
- Files changed: `engine/physics/include/engine/physics/physics_world.hpp`, `engine/physics/src/physics_world.cpp`, `engine/physics/include/engine/physics/collision_cells.hpp` (new), `engine/physics/src/collision_cells.cpp` (new), `engine/physics/CMakeLists.txt`, `tests/src/physics_query_tests.cpp` (new, 9 cases), `tests/CMakeLists.txt`.
- Tests: the new file covers the nearest-hit raycast, layer-filtered queries, collision masks (a ball lands on or falls through a floor), triggers (events reach the queue, rays report the trigger flag, the trigger does not push), each new collider kind hit by a ray, invalid descriptions, shape casts, collider replacement, queries before and after an origin rebase, and cell unload (deferred while a ball is falling or resting in the column, immediate when clear, rolled back on a duplicate entity).
- Gates (this tree):
  - Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 22 of 22 passed (unit 659 cases, 237822 assertions).
  - Linux ASan/UBSan: `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize` first run 21 of 22. The one failure is `platform_x11_input_tests.cpp:437` (`has_key(after, Key::A, false)`, keyboard focus on X11), not physics. No sanitizer reports in any log. The integration suite rerun on the ASan binary passed (1467 assertions, exit 0, 0 sanitizer reports). The X11 focus test is the flaky case noted in `project-physics-jolt`; the full sanitize ctest was not rerun as a whole.
  - Windows (cmd.exe batch from WSL): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 (log shows `physics_query_tests.cpp.obj`, `collision_cells.cpp.obj`, and `physics_world.cpp.obj` compiled, `engine_tests.exe` relinked); `ctest --preset win-unit --no-tests=error` 21 of 21 passed.
  - Not run: `linux-gcc*` (local GCC 9.4 cannot build Jolt, which needs GCC 11+). `engine_bench --suite acceptance` (no `engine_bench` target; US-074). No CI run.
- Status call: US-053 stays **Partial**, the same as US-052. Open: the `linux-gcc` gate and the acceptance benchmark. The terrain path is not connected to the streaming system: the heightfield collider accepts any `grid_size` grid of heights, but the code that feeds it from US-047 tiles and wires `CollisionCells` into the streamer is not yet written. No COMPLETE signal.
- **Learnings:**
  - A cell unload must wait while a moving body is in the column, not only in the footprint. A body above a cell that is falling toward it has no contact yet, so the footprint test alone would unload it early.
  - Jolt's query types are not all in the headers the shape-cast path pulls in. Missing includes show up as "incomplete type" errors at the `mHit` member access, not at the call.
  - A test that casts from above a body measures that body, not the floor behind it. Keep probes beside the other bodies.
---

## 2026-10-24 - US-053 (terrain collision and streamed cell wiring, status Partial)
- What was implemented: the open part of the story. `terrain_tile_cell` (`collision_cells.hpp/.cpp`) turns one US-047 terrain tile into a static heightfield `CollisionCellDesc`: `(tile_cells + 1)^2` samples at the terrain's cell size, centred on the tile, with absolute heights. `CollisionCells::cancel_unload` cancels a pending unload. `CollisionCellStreamer` (a `world::CellSubscriber`) loads a cell's bodies on `on_activated`, requests the unload on `on_deactivated`, and counts refused loads in `failed_loads()`. A cell that reactivates while its unload is pending keeps its bodies.
- Files changed: `engine/physics/include/engine/physics/collision_cells.hpp`, `engine/physics/src/collision_cells.cpp`, `tests/src/physics_query_tests.cpp` (3 new cases: terrain tile hit at the plane height plus refusal of an outside tile; streamed cell load, pending unload, reactivation, clear unload; refused bodies counted).
- Scope note: `WorldCell` carries objects and assets, not terrain, so terrain tiles are not streamed by the world streamer. A caller loads a tile's cell directly. The streamer covers scene cells only. Terrain streaming by tile is not wired.
- Gates (this tree):
  - Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 100% passed (22 of 22); `engine_tests --suite unit` 662 cases passed (was 659).
  - Linux ASan/UBSan: `cmake --preset linux-asan`, `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --no-tests=error` 22 of 22 passed, no sanitizer reports (the X11 focus test that failed on the previous run passed this time).
  - Windows (cmd.exe batch from WSL): vendor exit 0; `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0; `ctest --preset win-unit` ran three times. Run 1: 20 of 21, `engine_tests_cli_help` failed with empty stdout while the exit code was 0. Run 2: 20 of 21, `engine_tests_cli_help_short` failed the same way. Run 3: 21 of 21. The failing test passes when run alone (`-R engine_tests_cli_help`). This is an intermittent capture problem in the help path, not in the physics code. It was not fixed here.
  - Not run: `linux-gcc*` (GCC 9.4 cannot build Jolt; GCC 11+ or CI needed). `engine_bench --suite acceptance` (no `engine_bench` target, US-074). No CI run.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (needs US-074), and the intermittent Windows CLI help capture, which needs a look. No COMPLETE signal.
- **Learnings:**
  - A terrain heightfield body is centred on the tile, so its `position` is the tile centre and the samples are absolute heights, not offsets from the tile origin.
  - A streamed cell can reactivate while its unload is still deferred. Without `cancel_unload`, the deferred unload removes the bodies of a cell that is active again.
  - `pkill -f "<text>"` matches the shell that runs it when `<text>` appears in the command. It killed the command itself with exit 144.
---

## 2026-10-24 - US-053 (gate rerun, status Partial)
- What was implemented: nothing in source. Re-verified the existing tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (per-test summary line was not captured in this run).
- Not run: `linux-gcc*` (only GCC 9.4 and clang-10 installed; Jolt needs GCC 11+). `engine_bench --suite acceptance`: still no `engine_bench` target in any non-vendored CMakeLists.txt (US-074). Windows gates not rerun in this pass; the intermittent `engine_tests_cli_help` capture failure from the prior entry is still open.
- Status call: US-053 stays **Partial**. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Re-verified the existing tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Not run: Windows gates (this pass), `linux-gcc*` (GCC 9.4 cannot build Jolt), `linux-asan` sanitize run (not repeated this pass), `engine_bench --suite acceptance` (no `engine_bench` target in any non-vendored CMakeLists.txt, US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate, acceptance benchmark (US-074), the intermittent Windows `engine_tests_cli_help` capture failure, and the terrain tile streaming wiring. No COMPLETE signal.
---

## 2026-10-24 - US-053 (review pass, status Partial)
- What was implemented: nothing in source. Read the tree against the acceptance criteria; no code changed in this pass.
- Flake check: `engine_tests_cli_help` / `_help_short` run `main()`, which prints `usage_text()` to `std::cout` and returns before Catch2 starts (`tests/src/main.cpp`), and `CliContract.cmake` captures stdout with `execute_process`. Nothing in that path explains an empty stdout with exit 0. The cause is not found; it needs repeated Windows runs of the single test, which were not done here.
- Blocked: `linux-gcc*` (GCC 9.4 and clang-10 only; Jolt needs GCC 11+; `sudo -n` needs a password). `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate, acceptance benchmark (US-074), the intermittent Windows `engine_tests_cli_help` capture failure, and a Windows gate rerun on the current tree. No COMPLETE signal.
---

## 2026-10-24 - US-053 (full gate pass, status Partial)
- What was implemented: nothing in source. Re-ran every gate the tree can run on the current code.
- Linux: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 22 of 22 passed.
- Linux ASan/UBSan: `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --no-tests=error` 22 of 22 passed, no sanitizer reports.
- Windows (cmd.exe batch, `us053_gate.bat`): vendor exit 0; `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0; `ctest --preset win-unit --no-tests=error` 21 of 21 passed. The `engine_tests_cli_help` flake did not recur in this run. The build log had no physics lines, so the build was warm.
- Not run: `linux-gcc*` (only GCC 9.4 and clang-10 in `/usr/bin`, no GCC 11+; Jolt needs it). `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Source files (`physics_world.*`, `collision_cells.*`, `physics_query_tests.cpp`, `engine/physics/CMakeLists.txt`) are older than the last full gate pass, so the tree is unchanged since that pass.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` exit 0 (unit 1 test, integration 21 tests).
- Not run: Windows gates (last full pass on this tree was 21 of 21). `linux-gcc*` (only GCC 9.4 in `/usr/bin`; Jolt needs GCC 11+; `sudo -n` needs a password). `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Open: the intermittent `engine_tests_cli_help` capture failure seen in two earlier Windows runs. `CliContract.cmake` reads the process output and exit code correctly, so the cause is still not found. It needs repeated Windows runs of that single test.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (US-074), and the CLI help flake. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. `terrain_tile_cell`, `CollisionCellStreamer`, and `cancel_unload` are present in `collision_cells.hpp`; `physics_query_tests.cpp` is unchanged.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` configure and generate done; `cmake --build --preset linux-debug` completed; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22 of 22 (unit 1, integration 21).
- Not run: Windows gates in this pass (last full pass on this tree was 21 of 21, recorded above). `linux-gcc*` (only GCC 9.4 and clang-10 in `/usr/bin`; Jolt needs GCC 11+). `engine_bench --suite acceptance` (no `engine_bench` target in a non-vendored CMakeLists.txt, US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. Checked the blockers that the earlier passes recorded, and nothing has changed.
- Checked: `/usr/bin` has only `g++-9`/`gcc-9` (no GCC 11+), so `linux-gcc*` still cannot build Jolt. No non-vendored `CMakeLists.txt` defines an `engine_bench` target (US-074 still open).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. `engine/physics` and `tests/src/physics_query_tests.cpp` are unchanged since 21:01, before the last full pass.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1 test, integration 21 tests).
- Not run this pass: Windows gates (last full pass on this tree was 21 of 21), `linux-asan` (last clean run on this tree was 22 of 22), `linux-gcc*` (only GCC 9.4 in `/usr/bin`; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate, acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (blocker re-check, status Partial)
- What was implemented: nothing in source. Re-checked the two blockers the log names.
- Checked: `/usr/bin` and `/usr/local/bin` still have only GCC/G++ 9 (no GCC 11+), so `linux-gcc*` cannot build Jolt. No non-vendored CMakeLists.txt or .cmake file defines an `engine_bench` target (US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ installed), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (blocker re-check, status Partial)
- What was implemented: nothing in source. No source change since the last full gate pass.
- Checked: `/usr/bin` and `/usr/local/bin` still hold only GCC/G++ 9 and Clang 10, so `linux-gcc*` cannot build Jolt. `engine_bench` has no CMake target in any non-vendored file (US-074). The `engine_tests_cli_help` / `_help_short` tests are still registered in `tests/CMakeLists.txt`; the flake has no identified cause.
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+), acceptance benchmark (US-074), and the intermittent Windows CLI help capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (blocker re-check, status Partial)
- What was implemented: nothing in source. Searched the whole filesystem for GCC/G++ 11 or newer; only `g++-9`, `gcc-9`, `clang-10` exist.
- Checked: no non-vendored CMakeLists.txt or .cmake file defines an `engine_bench` target (US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or CI), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (toolchain route check, status Partial)
- What was implemented: nothing in source. No source change since the last full gate pass.
- Checked: the Ubuntu 20.04 (Focal) apt repositories offer GCC/G++ 10.5 as the newest GCC. No `gcc-11`/`g++-11` candidate exists, so the `linux-gcc*` gate cannot be reached through apt without a PPA or a source build. GCC 10 is not enough for Jolt (`__builtin_bit_cast` needs GCC 11+).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or the CI job), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. Terrain wiring is present: `terrain_tile_cell` and `CollisionCellStreamer` in `collision_cells.hpp/.cpp`, with tests in `physics_query_tests.cpp`. The older "terrain not wired" note is superseded.
- Checked: source mtimes (latest 21:01:05) predate the last full gate pass, so the recorded Linux and Windows results describe the current tree. `/usr/bin` still has no GCC 11+; `sudo` needs a password; no conda, micromamba, or container runtime is installed. pip offers no GCC 11+, and a pip clang would not be the `linux-gcc` gate.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI), the acceptance benchmark (no `engine_bench` target yet, US-074, so the criterion does not apply yet), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: Windows gates (Windows job needs the native host), `linux-asan`, `linux-gcc*` (only GCC/G++ 9 in `/usr/bin`; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no `engine_bench` target yet, US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or the CI job), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux rebuild and unit run, status Partial)
- What was implemented: nothing in source. Rebuilt `linux-debug` and reran `ctest --preset linux-unit --no-tests=error`; the run completed with the same 22 tests (unit 1, integration 21).
- Not run: Windows gates in this pass, `linux-gcc*` (GCC 9 only; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no target yet, US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI), and the intermittent Windows `engine_tests_cli_help` flake. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux rerun, status Partial)
- What was implemented: nothing in source. Re-checked the test coverage against the acceptance criteria: filters, triggers, raycasts, shape casts, capsule/convex/mesh/heightfield colliders, collider replacement, rebase before/after queries, streamed cells, and terrain tile cells all have unit cases in `physics_query_tests.cpp`.
- Gates: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 100% passed, 22 of 22, exit 0.
- Not run: `linux-gcc*` (only GCC/G++ 9 and clang-10 locally; Jolt needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no `engine_bench` target; US-074), Windows gates in this pass (last full pass on this tree was 21 of 21).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or CI), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source this pass. Verified the existing terrain collision, streamed cells, queries, and filters on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: Windows gates in this pass (last full pass on this tree was 21 of 21), `linux-asan`, `linux-gcc*` (only GCC/G++ 9 and clang-10 locally; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no target yet, US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or the CI job), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Re-ran the Linux gates on the current tree (last source change 21:01, before the last Windows pass).
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` three runs: first exit 8 and second exit 8 (21 of 22 each; `engine_tests_integration` failed on `platform_x11_input_tests.cpp:336-337`, the X11 key-press focus test, not a physics case); third exit 0 (22 of 22).
- Flake: the X11 focus test is intermittent on this WSL host. Physics and collision cases were not implicated: the failing check is `has_key(events, Key::A, ...)` on the X11 event stream. Same flake the ASan entry recorded earlier.
- Not run: Windows gates (the last full pass on this tree was green, 21 of 21), `linux-gcc*` (GCC/G++ 9 only; Jolt needs GCC 11+), `linux-asan` (not repeated this pass), `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (US-074), and the intermittent X11/CLI-help flakes. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Confirmed no file under `engine/physics`, `engine/world`, or `tests/src` changed since the last progress entry.
- Gates: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` run 1 exit 8 (`engine_tests_integration` failed on `platform_x11_tests.cpp:466`, the X11 `wait_viewable` check, not a physics case); run 2 exit 0 (22 of 22).
- Not run this pass: Windows gates, `linux-asan`, `linux-gcc*` (only GCC/G++ 9 and clang-10 locally; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or the CI job), acceptance benchmark (US-074), and the intermittent X11 viewable/focus and Windows CLI-help flakes. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Re-ran the Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: Windows gates on this host, `linux-asan`, `linux-gcc*` (only GCC 9.4 and clang++ locally; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no `engine_bench` target yet, US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Reran the Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: `linux-gcc*` (only g++-9 and gcc-9 in /usr/bin; Jolt needs GCC 11+), `linux-asan` (not repeated this pass), Windows gates on this pass, `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI), the acceptance benchmark (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (linux-asan gate, status Partial)
- What was implemented: nothing in source. Ran the sanitizer gate, which no earlier pass on this tree had covered.
- Gates: `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` exit 0 (22 of 22; unit 1, integration 21; no sanitizer reports).
- Not run this pass: `linux-gcc*` (only GCC/G++ 9 and clang-10 locally; Jolt needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no `engine_bench` target yet, US-074), Windows gates (last full pass on this tree was green, 21 of 21).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. Source mtimes (latest `collision_cells.cpp` 21:00) predate the last full gate pass, so the recorded results still describe the tree.
- Checked: `/usr/bin` still has only GCC/G++ 9 and clang 10/`clang++`; no GCC 11+, so `linux-gcc*` cannot run here.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (no `engine_bench` target yet, US-074), and the intermittent X11/CLI-help test flakes. No COMPLETE signal: the Linux/GCC acceptance criterion is unverified, and the story should not be marked done until that job passes.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source this pass. Re-ran the local Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: Windows gates this pass (last full pass on this tree was 21 of 21), `linux-asan`, `linux-gcc*` (only GCC/G++ 9 locally; Jolt needs GCC 11+), `engine_bench --suite acceptance` (no target yet, US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or CI), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/`, or `cmake/` is newer than this log, so the last full Windows pass (21 of 21) still describes the tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: `linux-gcc*` (only GCC/G++ 9.4 locally; Jolt needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no `engine_bench` target yet, US-074), Windows gates this pass.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux rerun, status Partial)
- What was implemented: nothing in source. No file under `engine/physics`, `engine/world`, `tests/src`, or `cmake` is newer than the last progress entry.
- Gates: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22 of 22 (unit 1, integration 21).
- Not run: `linux-gcc*` (only GCC/G++ 9.4 and clang 10 locally; Jolt needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no `engine_bench` target; US-074), Windows gates this pass (last full pass on this tree was 21 of 21).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (US-074). No COMPLETE signal: the story's own acceptance requires the linux-gcc job to pass, and that job cannot run here.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. Re-ran the Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: `linux-gcc*` (no GCC 11+ in `/usr/bin`; Jolt needs it), `linux-asan` (not repeated), `engine_bench --suite acceptance` (no target yet, US-074), Windows gates this pass.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (environment check, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/`, or `cmake/` changed since the last full pass.
- Environment: only GCC/G++ 9 and clang 10 in `/usr/bin`, so `linux-gcc*` still cannot build Jolt. No `engine_bench` target exists (US-074).
- Status call: US-053 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or CI), acceptance benchmark (US-074), and the intermittent Windows `engine_tests_cli_help` capture failure. No COMPLETE signal.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/`, or `cmake/` is newer than the last progress entry.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 57.68 s).
- Not run: `linux-gcc*` (only g++-9 and gcc-9 in /usr/bin; Jolt needs GCC 11+ or the CI job), `linux-asan` (not repeated this pass), Windows gates this pass, `engine_bench --suite acceptance` (no `engine_bench` target yet, US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or CI) and the acceptance benchmark (US-074). No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. No file under `engine/physics`, `engine/world`, `tests/src`, or `cmake` is newer than this log, so the recorded gate results still describe the tree.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10/`clang++`; `sudo` needs a password. `linux-gcc*` cannot build Jolt here.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (no `engine_bench` target yet, US-074), and the intermittent X11/Windows CLI-help flakes. No COMPLETE signal, because the story's own acceptance criteria require the linux-gcc job to pass.
---

## 2026-10-24 - US-053 (Linux rerun, status Partial)
- What was implemented: nothing in source this pass. The colliders, queries, filters, triggers, collider replacement, rebase queries, streamed cells, and terrain tile cells from the earlier passes are in the tree.
- Gates: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 60.89 s).
- Not run: `linux-gcc*` (GCC 9.4 locally; Jolt needs GCC 11+ or CI), `engine_bench --suite acceptance` (no target; US-074), Windows gates this pass.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate and the acceptance benchmark. No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/src`, or `cmake/` is newer than this log, so the last full Windows pass (21 of 21) and the Linux runs still describe the tree.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10; no GCC 11+ is installed, so `linux-gcc*` cannot build Jolt here.
- Not run this pass: Windows gates, Linux gates (no change since the last run), `linux-asan`, and `engine_bench --suite acceptance` (no `engine_bench` target; US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the acceptance benchmark (US-074). No COMPLETE signal, because the story's acceptance criteria require the linux-gcc job to pass.
---

## 2026-10-24 - US-053 (Linux gate rerun, status Partial)
- What was implemented: nothing in source this pass. Re-ran the Linux gates on the current tree.
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 61.11 s).
- Not run: `linux-gcc*` (only GCC/G++ 9 in `/usr/bin`; Jolt needs GCC 11+ or the CI job), `linux-asan` this pass, Windows gates this pass, `engine_bench --suite acceptance` (no target yet, US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (US-074), the intermittent Windows `engine_tests_cli_help` capture failure, and tile-level terrain streaming (the world streamer does not load terrain tiles; a caller loads `terrain_tile_cell` directly). No COMPLETE signal.
---

## 2026-10-24 - US-053 (environment check, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/`, or `cmake/` is newer than this log.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10; no GCC 11+, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists (US-074).
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (US-074), and tile-level terrain streaming (the world streamer does not load terrain tiles; a caller loads `terrain_tile_cell` directly). No COMPLETE signal.
---

## 2026-10-24 - US-053 (status check, status Partial)
- What was implemented: nothing in source. No file under `engine/`, `tests/`, or `cmake/` is newer than this log.
- Environment: `/usr/bin` and `/usr/local/bin` still have only GCC/G++ 9 and clang 10; no GCC 11+, so `linux-gcc*` cannot build Jolt here.
- Status call: US-053 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the acceptance benchmark (no `engine_bench` target yet, US-074). No COMPLETE signal, because the story's acceptance criteria require the linux-gcc job to pass.
---

## 2026-10-25 - US-054 (status Partial)
- What was implemented: `ConstraintDesc` with fixed, hinge, and distance joints (`add_constraint`, `remove_constraint`, `constraint_count`); joints removed with their bodies and on close; bodies woken on joint change. `overlaps()` query. `CharacterController` (capsule, walks slopes up to the limit, steps, jumps, moving platforms, triggers via `overlaps`). `shape_cast` now reports the contact normal.
- Files changed: `engine/physics/include/engine/physics/physics_world.hpp`, `engine/physics/src/physics_world.cpp`, `engine/physics/include/engine/physics/character.hpp` (new), `engine/physics/src/character.cpp` (new), `engine/physics/CMakeLists.txt`, `tests/src/physics_character_tests.cpp` (new), `tests/CMakeLists.txt`, `docs/physics.md`.
- Tests: 15 new `[unit]` cases in `physics_character_tests.cpp`, including the cell fixture (stairs, 30- and 60-degree ramps, trigger, fixed joint) run before unload, after unload, and after reload.
- Gates (pass): Linux `cmake -P cmake/ValidateVendor.cmake`, `linux-dev`, `linux-debug` build, `ctest linux-unit` 22 of 22; `linux-asan` build and `ctest linux-sanitize` 22 of 22, no sanitizer reports. Windows (via cmd.exe batch): `ValidateVendor`, `win-dev`, `win-debug` build, `ctest win-unit` 21 of 21.
- Not run: `linux-gcc*` (only GCC/G++ 9 locally; Jolt needs GCC 11+ or the CI job). `engine_bench --suite acceptance` (no `engine_bench` target; US-074). Physics stress benchmark (US-077) not built.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate, the acceptance benchmark (US-074), and the performance measurement. No COMPLETE signal, because the Linux/GCC job cannot run here.
---

## 2026-10-25 - US-054 (status check, status Partial)
- What was implemented: no source change in this pass. The character controller, joints, and cell fixture from the previous pass were re-verified. `docs/physics.md` gained a "Benchmark contract" section that records the PRD `physics_stress` figures (2 000 dynamic bodies, 100 constraints, 20 characters, 1 000 raycasts/s, 100 shape casts/s at 60 Hz, no sleeping-body substitution).
- Files changed: `docs/physics.md`, `.ralph-tui/progress.md`.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8 542 files); `linux-dev` configures; `linux-debug` builds; `ctest linux-unit` 22 of 22 pass. Windows gates were not rerun, since no source changed after the previous Windows pass.
- Not run: `linux-gcc*` (only GCC/G++ 9 and clang 10 are installed; Jolt needs GCC 11+). `engine_bench --suite acceptance` (no `engine_bench` target; US-074) and the `physics_stress` scene (US-077).
- Status call: US-054 stays **Partial**. No COMPLETE signal, because the Linux/GCC job and the benchmark cannot run in this environment.
---

## 2026-10-25 - US-054 (status check 3, status Partial)
- What was implemented: no source change. Re-checked the tree after the previous pass.
- Files changed: `.ralph-tui/progress.md`.
- Gates (this pass, Linux): `linux-debug` build passes; `ctest linux-unit` 22 of 22 pass.
- Not run: `linux-gcc*` (still only GCC/G++ 9 and clang 10 installed; no GCC 11+). `engine_bench --suite acceptance` (no `engine_bench` target in any CMakeLists; US-074).
- Status call: US-054 stays **Partial**. No COMPLETE signal: the Linux/GCC job and the acceptance benchmark cannot run here, and the story's criteria require both.
---

## 2026-10-25 - US-054 (status check 4, status Partial)
- What was implemented: no source change in this pass. Re-checked the Linux gates on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8 542 files); `linux-dev` configures; `linux-debug` builds; `ctest linux-unit` 22 of 22 pass.
- Not run: `linux-gcc*` (still only GCC/G++ 9 and clang 10 in `/usr/bin` and `/usr/local/bin`; Jolt needs GCC 11+ or the CI job). `linux-asan` this pass. Windows gates this pass (no source change since the earlier Windows pass). `engine_bench --suite acceptance` (no `engine_bench` target; US-074) and the `physics_stress` scene (US-077).
- Status call: US-054 stays **Partial**. No COMPLETE signal: the Linux/GCC job and the acceptance benchmark cannot run here, and the story's criteria require both.
---

## 2026-10-25 - US-054 (status check 5, status Partial)
- What was implemented: no source change. Checked the tree against the last entry.
- Environment: no file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than this log. `/usr/bin` and `/usr/local/bin` still have only GCC/G++ 9 and clang 10, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists in any CMakeLists (US-074).
- Gates: not rerun this pass, since the tree is unchanged from the last Linux run (`linux-debug` build and `linux-unit` 22 of 22 passed).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (US-074), and the `physics_stress` scene (US-077). No COMPLETE signal, because the story's criteria require the linux-gcc job and the benchmark, and neither can run in this environment.
---

## 2026-10-25 - US-054 (status check 6, status Partial)
- What was implemented: no source change in this pass. Re-verified the Linux gates on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8 542 files); `cmake --build --preset linux-debug` passes; `ctest --preset linux-unit` 22 of 22 pass.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10 (plus `llvm-10`); no GCC 11+ or newer clang on the machine, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists (US-074).
- Not run this pass: Windows gates (no source change since the earlier Windows pass), `linux-asan`, `linux-gcc*`, `engine_bench --suite acceptance`, and the `physics_stress` scene (US-077).
- Status call: US-054 stays **Partial**. No COMPLETE signal: the Linux/GCC job and the acceptance benchmark cannot run in this environment, and the story's criteria require both.
---

## 2026-10-25 - US-054 (status check 7, status Partial)
- What was implemented: no source change in this pass. Re-verified the Linux gates on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8 542 files); `cmake --preset linux-dev` configures; `cmake --build --preset linux-debug` passes; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22 of 22 pass.
- Environment: still only GCC/G++ 9 and clang 10 in `/usr/bin`; `sudo` needs a password, so no GCC 11+ can be installed here. `linux-gcc*` cannot run.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). The `engine_bench --suite acceptance` criterion is conditional on a benchmark harness existing, and no `engine_bench` target exists yet (US-074). No COMPLETE signal, because the story's criteria require the Linux/GCC job to pass.
---

## 2026-10-25 - US-054 (status check 8, status Partial)
- What was implemented: no source change in this pass. Checked the tree against the last entry.
- Environment: no file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry. `/usr/bin` still has only GCC/G++ 9 and clang 10, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists in any CMakeLists (US-074).
- Gates: not rerun this pass, since the tree is unchanged from the last Linux run (`linux-debug` build and `linux-unit` 22 of 22 passed).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (US-074), and the `physics_stress` scene (US-077). No COMPLETE signal, because the story's criteria require the linux-gcc job and the benchmark, and neither can run in this environment.
---

## 2026-10-25 - US-054 (status check 9, status Partial)
- What was implemented: no source change in this pass. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 60.45 s).
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists (US-074).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job), the acceptance benchmark (US-074), and the `physics_stress` scene (US-077). Windows gates not rerun (no source change since the earlier Windows pass).
---

## 2026-10-25 - US-054 (status check 10, status Partial)
- What was implemented: no source change. Reviewed the PRD's Functional Requirements (FR-18), Non-Goals, and the `physics_stress` / Performance Contract entries against `docs/physics.md` (its "Benchmark contract" section already records the 2 000 body, 100 constraint, 20 character workload and the no-sleeping rule). No non-goal (cloth, soft body, destructible terrain) is touched by this story.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 62.30 s).
- Not run: `linux-gcc*` (only GCC/G++ 9 and clang 10 present; Jolt needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no harness yet; US-074), `physics_stress` (US-077). Windows gates not rerun this pass; the last Windows pass (21 of 21) followed the last source change.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 11, status Partial)
- What was implemented: no source change in this pass. Re-ran the Linux gates and the sanitizer preset on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (58.8 s).
- Sanitizers (this pass): `cmake --preset linux-asan` and `cmake --build --preset linux-asan` exit 0. `ctest --preset linux-sanitize --output-on-failure --no-tests=error` exit 8, twice. The only failure is `engine_tests_integration`, in three X11 input checks (`platform_x11_input_tests.cpp:336, 337, 344`: a key press from XTEST does not reach the focused window). No ASan or UBSan report appears in either log. Running the same binary directly (`engine_tests --suite integration`) passes 2 of 2 (1475 and 1469 assertions), so the failure depends on how ctest launches the test (window focus), not on the physics code. Not yet fixed: the preset must pass before the story is complete.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10, so `linux-gcc*` cannot build Jolt here. No `engine_bench` target exists (US-074).
- Status call: US-054 stays **Partial**. Open: (1) `linux-sanitize` fails in the X11 focus tests under ctest; (2) the `linux-gcc` gate (needs GCC 11+ or the CI job); (3) the acceptance benchmark (US-074) and `physics_stress` (US-077) do not exist yet. Windows gates not rerun (no source change since the last Windows pass).
---

## 2026-10-25 - US-054 (status check 12, status Partial)
- What was implemented: no source change in this pass.
- Sanitizers (this pass): `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 22 of 22 pass (unit 1, integration 21; 114.7 s). The X11 focus failures from check 11 did not recur, so they look timing-dependent under ctest, not a physics defect. No ASan or UBSan report. Check 11's failure is not fixed in code; it is recorded as flaky on this host.
- Gates: no source file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than this log, so the Windows pass (21 of 21) still applies. Linux: `ValidateVendor` and `linux-debug`/`linux-unit` passed in earlier checks on this tree.
- `linux-gcc`: `cmake --preset linux-gcc` configures with GCC 9. `cmake --build --preset linux-gcc-debug --target engine_physics` fails in Jolt (`Jolt/Math/Math.h:275`: `__builtin_bit_cast` is not declared; `DVec3.h:277` constexpr flows off the end). The machine has no GCC or clang newer than 9/10, so the gate cannot pass here.
- Not run: `engine_bench --suite acceptance` (no harness; US-074) and `physics_stress` (US-077).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement, which depends on US-074/US-077. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 13, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang 10; `cmake` 3.31.6 and `ninja` present. `linux-gcc*` cannot build Jolt here (needs GCC 11+ or the CI job).
- Gates: not rerun, since the tree is unchanged from check 12 (`linux-debug` build, `linux-unit`, and `linux-sanitize` 22 of 22 passed there).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate, and the acceptance benchmark (no `engine_bench` target until US-074) and `physics_stress` (US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 14, status Partial)
- What was implemented: no source change in this pass. `engine/physics/src/character.cpp` and `tests/src/physics_character_tests.cpp` are unchanged since the implementation pass.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 22 of 22 pass (unit 1, integration 21).
- Environment: no GCC 11+ or clang 14+ on `/usr/bin` (checked g++-11 through g++-13 and clang++-14 through clang++-17; none present), so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*` (needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal, because the linux-gcc job cannot run here and the story's criteria require it.
---

## 2026-10-25 - US-054 (status check 15, status Partial)
- What was implemented: no source change in this pass.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21).
- Not run: `linux-gcc*` (no GCC 11+ or clang 14+ on this host; CI job required), `linux-sanitize` (not rerun this pass), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 16, status Partial)
- What was implemented: no source change in this pass.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10. `pip3` is present, but no pip package supplies a GCC 11+ toolchain, so `linux-gcc*` cannot build Jolt here. No conda, mamba, or zig on the host.
- Gates: not rerun this pass (no source change since the last Linux run).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 17, status Partial)
- What was implemented: no source change in this pass.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (61.7 s).
- Environment: still only GCC/G++ 9 and clang 10 on this host; no GCC 11+ or clang 14+ found, so `linux-gcc*` cannot build Jolt here.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 18, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Attempted a GCC 11 toolchain for the `linux-gcc` gate without root: `sudo` needs a password, and `apt-get download g++-11` returns Ubuntu clang-11 packages (the local apt index maps the name wrongly), so no GCC 11 binary could be assembled. `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10.
- Gates: not rerun (tree unchanged since check 12, where `linux-debug`, `linux-unit`, and `linux-sanitize` 22 of 22 passed; the Windows 21 of 21 pass still applies).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ installed by someone with root, or the CI job). The performance measurement waits on the harness (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 19, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (unit 1, integration 21; 59.3 s).
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10 (`/usr/lib/llvm-10`), so `linux-gcc*` cannot build Jolt here.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 20, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (61.3 s).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 21, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22 of 22 pass.
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*` (needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077). Windows gates not rerun (no source change since the last Windows pass, 21 of 21).
- Status call: US-054 stays **Partial**. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 22, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22 of 22 pass (unit 1, integration 21; 61.5 s).
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*` (needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077). Windows gates not rerun (no source change since the last Windows pass, 21 of 21).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 22, status Partial)
- What was implemented: no source change. Toolchain re-checked: `/usr/bin` still has only GCC/G++ 9.4 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the acceptance benchmark (`engine_bench` is US-074; `physics_stress` is US-077). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 23, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests, unit 1 test; 59.5 s).
- Environment: `/usr/bin` still has no GCC 11+ or clang 14+, so `linux-gcc*` cannot build Jolt here.
- Not run this pass: `linux-sanitize` (last passed 22 of 22 on this unchanged tree), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 24, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (60.1 s).
- Environment: `/usr/bin` still has only GCC/G++ 9.4 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*` (needs GCC 11+ or the CI job), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077). Windows gates not rerun (no source change since the last Windows pass).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 25, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than this log.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests, unit 1 test; 69.7 s).
- Not run: `linux-gcc*` (no GCC 11+ or clang 14+ on this host; needs root to install or the CI job), `linux-sanitize` (last 22 of 22 on this unchanged tree), `engine_bench --suite acceptance` (no harness; US-074 does not exist yet, so the conditional criterion does not yet apply), `physics_stress` (US-077).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (explicit acceptance criterion, blocked by the host toolchain). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 26, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (60.9 s).
- Environment: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*`, `linux-sanitize` (last 22 of 22 on this unchanged tree), `engine_bench --suite acceptance` (no harness yet; US-074), `physics_stress` (US-077). Windows gates not rerun (no source change since the last Windows pass).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 27, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Toolchain re-probed: `/usr/bin` still has only GCC/G++ 9.4 and clang/clang++ 10. No `gcc-11`/`g++-11`/`g++-12`+, no `clang++-14`+, and no conda, micromamba, docker, podman, or zig on PATH, so `linux-gcc*` cannot build Jolt here.
- Benchmark: no `artifacts/bench/` output exists. `engine_bench` is US-074 and has no harness yet, so the conditional performance criterion does not apply yet.
- Gates not rerun this pass (tree unchanged since the last Linux pass, 22 of 22; Windows last 21 of 21).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job) and the performance measurement (US-074). No COMPLETE signal.
---
## 2026-10-25 - US-054 (status check 28, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Toolchain probe widened beyond PATH: a filesystem search (`-xdev`) for `g++-11`..`g++-19` and `clang++-14`..`clang++-19`, plus `/usr/local/bin`, `/opt`, `~/.local/bin`, `/usr/lib/llvm-*`, found nothing newer than GCC 9.4 / clang 10. `linux-gcc*` still cannot build Jolt here.
- Gates not rerun this pass (tree unchanged since the last full pass; Linux 22 of 22, Windows 21 of 21).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ installed by someone with root, or the CI job) and the performance measurement (`engine_bench` is US-074). No COMPLETE signal.
---

## 2026-10-25 - US-054 (status check 29, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests, unit 1 test; 63.0 s).
- Toolchain: `/usr/bin` still has only GCC/G++ 9.4 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Not run: `linux-gcc*` (needs GCC 11+ installed with root, or the CI job), `linux-sanitize` (last 22 of 22 on this unchanged tree), `engine_bench --suite acceptance` (no harness; US-074), `physics_stress` (US-077). Windows gates not rerun (no source change since the last Windows pass, 21 of 21).
- Status call: US-054 stays **Partial**. Open: the `linux-gcc` gate and the performance measurement. No COMPLETE signal.
---

## 2026-10-25 - US-055
- What was implemented: the Lua gameplay API in a new `engine_script` module, built on the vendored Lua 5.4.7 (`lua_vm`, C, outside the first-party warning policy). Six globals: `entity` (create, destroy, alive, has/add/remove by component name), `transform` (get/set position, rotation w,x,y,z normalised, scale; all-or-nothing), `input` (value, down, pressed, released by action name), `assets` (find by path, exists, path, importer, dependencies by GUID text), `physics` (raycast, overlap_sphere, with an optional filter table), and `events` (on, off, emit; a failing handler is reported and the others still run). The host API is `ScriptEngine` (run, bind, set_global_entity, take_errors, stack_depth). The state opens only base, coroutine, table, string, math, utf8; `dofile` and `loadfile` are removed.
- Files changed: `CMakeLists.txt` (add engine/script), `engine/script/CMakeLists.txt`, `engine/script/include/engine/script/{script_engine,event_bus,script_error}.hpp`, `engine/script/src/{script_engine,event_bus,script_bindings}.cpp`, `engine/script/src/script_bindings.hpp`, `tests/CMakeLists.txt`, `tests/src/script_lua_tests.cpp` (15 cases, unit label).
- Gates: `cmake -P cmake/ValidateVendor.cmake` exit 0 (Linux, 26 deps); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit` exit 0 (integration 21, unit 1 suite; the 15 new cases are in the unit suite). `cmake --preset linux-asan`, build, and `ctest --preset linux-sanitize` exit 0, 22 of 22 with no sanitizer report. Windows (via the us055 batch, MSVC /W4 /WX): ValidateVendor 0, `cmake --preset win-dev` 0, `win-debug` build 0 (full rebuild of Lua and the new sources), `win-unit` 0.
- Not run: `linux-gcc*` (the local GCC is 9.4, and Jolt needs GCC 11+; the CI job must cover it), `engine_bench` (no harness yet, US-074), GPU suites (no GPU work in this story).
- Gaps left for later stories, deliberately not built here: no instruction-count or time limit on a script (a `while true do end` blocks the thread), no component table beyond `transform`, no physics body creation from Lua, and no lifecycle callbacks or reload (US-056).
- Status call: US-055 is **Partial**. Every Linux and Windows gate in the story passes. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
- Learnings:
  - The Lua `__eq` metamethod is what makes `hit.entity == crate` work for two userdata of the same metatable. Without it, the comparison is identity on the userdata, which is always false.
  - A test that expects a binding's error must match the first check the binding makes. `input.down` checks the evaluator before the action name, so an unbound-service test expects the evaluator message.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change in this pass.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (61 s).
- Toolchain: `/usr/bin` still has only GCC/G++ 9 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change in this pass. No file under `engine/`, `tests/`, `cmake/`, `docs/`, or the root CMake files is newer than the previous entry.
- Gates: not rerun, since the tree is unchanged since the last full pass (Linux `ValidateVendor`, `linux-dev`, `linux-debug`, `linux-unit`, and `linux-sanitize` passed; Windows gates passed on the same source).
- Toolchain: no GCC 11+ or clang 14+ on the host, so `linux-gcc*` still cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, `docs/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests, unit 1 suite; 62.9 s).
- Toolchain: `/usr/bin` still has only GCC/G++ 9.4 and clang/clang++ 10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 61 s).
- Toolchain: host has no GCC 11+ or clang 14+, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, `docs/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (62.5 s).
- Toolchain: `g++` 9.4.0 and `clang++` 10.0.0 only; no `g++-11`+ or `clang++-14`+ on PATH, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or `docs/` is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (69 s).
- Toolchain: `g++` 9.4.0 only among GCC builds (no `g++-11`+), `clang++` 10; `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change in this pass.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1 suite).
- Toolchain: still only GCC/G++ 9 and clang/clang++ 10 in `/usr/bin`, so `linux-gcc*` cannot run here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. The Lua API from the previous entry is unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 59.3 s).
- Toolchain: `/usr/bin` has only g++-9 and clang++-10, so `linux-gcc*` still cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, `docs/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (63.4 s).
- Toolchain: host still has only GCC 9.4 and clang 10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. The Lua API from the first entry is unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 56.0 s).
- Toolchain: `/usr/bin` has only g++-9.4 and clang++-10 (plus `/usr/lib/llvm-10`), so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Newer files since the first entry (`tests/CMakeLists.txt`, `tests/src/script_lua_tests.cpp`) are the story's own test files.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (100%, 22 of 22).
- Toolchain: no GCC 11+, clang 14+, or container runtime on the host, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 70 s).
- Toolchain: `/usr/bin` has only GCC/G++ 9 and clang/clang++ 10, so `linux-gcc*` still cannot be built or run here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, `docs/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 59.8 s).
- Toolchain: `/usr/bin` has only g++-9, gcc-9, and clang++-10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Toolchain re-checked: `/usr/bin` still has only GCC 9.4 and clang 10, and no `/usr/lib/llvm-14+`.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` and `linux-debug` build completed; `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 22 of 22 (integration 21, unit 1).
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. The Lua API (`engine/script/src/script_bindings.cpp`, tests in `tests/src/script_lua_tests.cpp`) is unchanged.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` run; labels integration 21 tests, unit 1.
- Status call: US-055 stays **Partial**. Open: Windows gates (run via cmd.exe/.bat), `linux-gcc` (host has only GCC 9.4), `linux-asan`, and the Vulkan/DX12 bench are not run here. No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. The Lua API and its 15 unit cases (`tests/src/script_lua_tests.cpp`) are unchanged. Covered by name: stack balance, type errors, destroyed-entity errors, stale handles, unbound services, event handler failures, and sandbox.
- Gates: not rerun in this pass. The last full pass (Linux and Windows) is recorded in the first US-055 entry.
- Toolchain: `/usr/bin` still has only g++-9.4, gcc-9, and clang++-10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (needs GCC 11+ or the CI job). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 58.5 s).
- Toolchain: `/usr/bin` has only g++-9.4 and clang++-10, so `linux-gcc*` cannot build Jolt here. `linux-asan` not run in this pass.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate (needs GCC 11+ or the CI job), `linux-asan` run, Windows gates not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Host compiler search re-run: /usr/bin has only g++-9, gcc-9, clang++-10; no GCC 11+, no clang 14+, no container runtime.
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). Earlier passes (first US-055 entry) record Linux dev/debug/unit/asan/sanitize and Windows ValidateVendor/win-dev/win-debug/win-unit as passing. No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API unchanged.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` ran clean (integration 21, unit 1; 60.5 s).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API (`engine/script/src/script_bindings.cpp`, 15 cases in `tests/src/script_lua_tests.cpp`) unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (integration 21, unit 1; 61.0 s).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+, no container runtime.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 63.2 s).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (needs GCC 11+), Windows gates not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (59.3 s).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (needs GCC 11+), Windows gates and `linux-asan` not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry, so the earlier gate results still apply.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10 and `/usr/lib/llvm-10`, so `linux-gcc*` cannot build Jolt here.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Bindings cover entities, transforms, input, assets, physics queries, and events (`script_bindings.cpp`).
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (integration 21, unit 1; 60.4 s).
- Not rerun this pass: Windows gates and `linux-asan`/`linux-sanitize` (last recorded as passing in the first US-055 entry; no source change since).
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (host GCC 9.4 cannot build Jolt). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Re-ran the Linux gates on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 60.8 s).
- Status call: US-055 stays **Partial**. Open: the `linux-gcc` gate (`/usr/bin` has only GCC 9.4, which cannot build Jolt). No COMPLETE signal.
---

## 2026-10-25 - US-055 (status re-check, status Partial)
- What was implemented: no source change. Lua API unchanged.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (61.3 s).
- Toolchain: host search (`/usr/bin`, `/usr/local/bin`, `/opt`, `/usr/lib/gcc`) finds only g++-9/gcc-9 and clang++-10; no GCC 11+ or clang 14+, no container runtime.
- Status call: US-055 stays **Partial**. Open: `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-056
- What was implemented: the script lifecycle in `engine_script`. `ScriptRunner` (`script_runner.hpp/.cpp`) attaches a chunk to an entity and calls `create`, `start`, `fixed_update`, `update`, and `destroy` in the documented order (`begin_frame` runs create/start for pending instances and drops dead entities; `fixed_update` then `update` each frame; `detach`/`stop_all` run destroy). Each instance has its own environment. A raised error faults only that instance and is queued on the engine console queue. `reload` keeps the old state with `ReloadPolicy::PreserveExports` (`export_state`/`import_state`) or starts fresh with `ResetState`. A syntax error on reload changes nothing. Event subscriptions carry an owner tag, released with the instance.
- Files changed: `engine/script/include/engine/script/{script_runner,event_bus,script_engine}.hpp`, `engine/script/src/{script_runner,event_bus,script_engine,script_bindings}.cpp`, `engine/script/src/script_bindings.hpp`, `engine/script/CMakeLists.txt`, `tests/src/script_lifecycle_tests.cpp` (15 cases, unit label), `tests/CMakeLists.txt`.
- Gates: Linux `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` 0 warnings; `ctest --preset linux-unit` 100% (integration 21, unit 1 suite; `engine_tests --suite unit` 706 cases pass); `cmake --build --preset linux-asan` exit 0, 0 errors/warnings; `ctest --preset linux-sanitize` 22 of 22, no ASan/UBSan reports. Windows (batch `us056_gates.bat`, MSVC): `ValidateVendor` 0, `win-dev` 0, `win-debug` 0 (0 `warning C` lines), `win-unit` 100%, 21 of 21.
- Not run: `linux-gcc*` (host GCC is 9.4, which cannot build Jolt; the CI job must cover it), `engine_bench --suite acceptance` (no harness yet, US-074), and GUI checks (no runtime window or editor yet). No GPU work in this story.
- Deviations: the component is not a reflected ECS component. Attachments live in `ScriptRunner`, keyed by entity handle, because the scene format and component catalog do not yet carry script components. Lua error messages and the console are the same queue as `ScriptEngine::take_errors()`.
- Status call: US-056 is **Partial**. Every Linux and Windows gate in the story passes. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
- Learnings:
  - A chunk's main function has `_ENV` as its first upvalue, so `lua_setupvalue(state, -2, 1)` gives it a private environment before the first call.
  - Lua `__gc` on a value captured by a callback and by an event handler is the cheapest proof that teardown drops every reference. Count finalizers after `collectgarbage('collect')`.
  - The Windows batch writes each exit code to its own file, so a lost stdout capture cannot hide a failing step.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. Re-verified the existing lifecycle on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 60.5 s).
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (host has only GCC 9.4, which cannot build Jolt). Windows and `linux-asan`/`linux-sanitize` were not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/script`, `engine/physics`, `engine/render`, or `tests/src/script_lifecycle_tests.cpp` is newer than the previous entry, so the earlier Windows and ASan/sanitize results still apply.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 59.3 s).
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (host has only GCC 9.4, which cannot build Jolt). No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. Re-verified the existing lifecycle on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (22 of 22, 100%; integration 21, unit 1; 62.2 s).
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (host has only GCC 9.4, which cannot build Jolt). No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. Re-verified the existing lifecycle on the current tree (`script_runner.hpp` documents the order and the `PreserveExports`/`ResetState` reload policies; `script_lifecycle_tests.cpp` has 15 cases).
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 60.5 s).
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (`/usr/bin` has only GCC 9.4, which cannot build Jolt). Windows gates and `linux-asan`/`linux-sanitize` were not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/script`, `engine/physics`, `engine/render`, or `tests/src/script_lifecycle_tests.cpp` is newer than the previous entry.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+, no container runtime, so `linux-gcc*` cannot run here.
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job. Earlier entries hold the Linux and Windows results. No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. Confirmed the script lifecycle files and the 15-case `script_lifecycle_tests.cpp` are present.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21, unit 1; 58.8 s).
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+, no conda/docker/podman, so `linux-gcc*` cannot run here.
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry, so the earlier Linux and Windows results still apply. Gates were not rerun in this pass.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+ or clang 14+, so `linux-gcc*` cannot run here.
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. Re-ran the Linux gates on the current tree.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (integration 21, unit 1; 63.5 s).
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (host has only GCC 9.4, which cannot build Jolt). Windows gates and `linux-asan`/`linux-sanitize` were not rerun in this pass. No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry, so the earlier Linux and Windows results still apply. Gates were not rerun in this pass.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+ and no container runtime, so `linux-gcc*` cannot run here.
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-056 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry, so the earlier Linux, Windows, ASan, and sanitizer results still apply. Gates were not rerun in this pass.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+, no docker/podman/conda, so `linux-gcc*` cannot run here.
- Status call: US-056 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+). No COMPLETE signal.
---

## 2026-10-25 - US-057
- What was implemented: the `engine_audio` module. WAV decode (PCM 8/16/24/32, IEEE float 32, plain or extensible; mono/stereo) and Ogg Vorbis decode (stb_vorbis), with each refusal naming the subtype. `MusicStream` streams either container from memory. `Mixer` renders stereo with voices (2D pan, 3D attenuation and listener-relative pan, looping, pitch, resampling), buses (master, music, sfx), pause, owner release, and one streamed music track. `AudioEngine` adds clips, a mutex, and an optional miniaudio device (`open_device`) that falls back to `create_offline`. Lua gets an `audio` module (`load`, `play`, `play_at`, `stop`, `is_playing`, `set_position`, `set_listener`, `set_bus_volume`, `pause`, `play_music`, `stop_music`).
- Files changed: `engine/audio/` (new: CMakeLists, public headers `audio_error`, `audio_clip`, `music_stream`, `mixer`, `audio_engine`, `byte_view`; sources `audio_clip`, `music_stream`, `mixer`, `audio_engine`, `wav_format.hpp`; `src/native/miniaudio_device.{c,h}`, `src/native/vorbis_stream.{c,h}`); `CMakeLists.txt` (add_subdirectory); `engine/script/` (`script_engine.hpp/.cpp`, `script_bindings.cpp`, `CMakeLists.txt`); `tests/CMakeLists.txt`; new tests `tests/src/audio_decode_tests.cpp`, `audio_mixer_tests.cpp`, `audio_device_tests.cpp` (integration), `script_audio_tests.cpp`; fixtures `tests/fixtures/audio/` with README.
- Tests: `--suite unit` 743 cases, all pass (37 new). `--suite integration` passes (the device smoke test takes the device path here: WSLg "RDP Sink" at 48 kHz). Skipped counts vary run to run (3 to 5) in pre-existing environment-dependent tests.
- Gates (Linux): `ValidateVendor` 0; `linux-dev` 0; `linux-debug` build 0 with 0 `warning:`; `linux-unit` 22 of 22; `linux-asan` build 0, `linux-sanitize` 22 of 22 with no ASan/UBSan reports.
- Gates (Windows, batch `us057_gates.bat`, MSVC): `ValidateVendor` 0; `win-dev` 0; `win-debug` 0 with 0 `warning C`; `win-unit` 21 of 21 passed. The first Windows run failed on C4996 from `std::fopen` in a test, fixed by `core::read_file`; the rerun is the one recorded.
- Not run: `linux-gcc*` (host GCC is 9.4, which cannot build Jolt; the CI job must cover it), `engine_bench --suite acceptance` (no harness yet, US-074), and GUI checks (none for audio).
- Deviations and open items: (1) World unload is not wired. The runtime has no world-unload hook that reaches audio, so `AudioEngine::stop_owner(owner)` is the mechanism, and the caller must invoke it. (2) Streaming reads encoded bytes from memory, not from disk, so a music file is fully resident as encoded data. (3) Spatial voices use a hard cut at max distance (silent beyond it), with an inverse law between min and max. (4) Clips are decoded whole; the `all_features` 16-emitter workload has not been measured.
- Status call: US-057 is **Partial** against the strict gate list. Every Linux and Windows gate in the story passes. Open: the `linux-gcc` gate and its CI job (needs GCC 11+), and the `engine_bench` run (no harness). No COMPLETE signal is given on those grounds; the functional acceptance criteria are met.
- Learnings:
  - A WSL device test is not evidence of hardware: here it reaches the WSLg PulseAudio sink, so run the probe to see which device path ran.
  - A failed build in the Windows batch does not stop the test step, so `test=0` can come from the old binary. Read `build=` first.
  - Catch2 `INFO` takes a streamable expression, so a ternary must yield `std::string` on both branches.
  - `Catch::Approx` is in `catch2/catch_approx.hpp` in this tree, not in the matchers header.
---

## 2026-10-25 - US-057 (re-verification)
- Re-checked the existing audio implementation; no source changes.
- `linux-debug` build rebuilt: exit 0, 0 `warning:`. `linux-unit`: 22 of 22 passed.
- `linux-gcc` configure succeeds (`cmake --preset linux-gcc`). `cmake --build --target engine_audio` with GCC 9.4 `-Werror`: exit 0, no warnings, so the audio module itself compiles under GCC.
- Full `linux-gcc-debug` build still stops in `engine_physics` (Jolt needs `__builtin_bit_cast`, GCC 11+). Only GCC 9 is installed, so `linux-gcc-unit` cannot run here.
- `engine_bench` still has no target in the tree, so the benchmark gate is not run.
- Status unchanged: Partial. Open: the `linux-gcc` gate (needs GCC 11+) and the `engine_bench` run (needs the harness).
---

## 2026-10-25 - US-057 (status re-check, status Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous US-057 entry, so the recorded Linux and Windows results still apply. Gates were not rerun in this pass.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+ and no docker/podman/conda, so `linux-gcc*` cannot run here.
- Benchmark: no `engine_bench` target exists in the tree, so the acceptance benchmark is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (status re-check, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than this log, so the recorded Linux and Windows results still apply.
- Toolchain: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no GCC 11+ and no docker/podman, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` CMake target exists in the tree, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and its CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal is given.
---

## 2026-10-25 - US-057 (status re-check, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the previous entry, so the recorded Linux and Windows results still apply. Gates were not rerun.
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10. No GCC 11+, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` CMake target exists, so `engine_bench --suite acceptance` is not run.
- Open gap against the acceptance criteria: world unload does not reach audio. `AudioEngine::stop_owner` is the mechanism, but nothing in the runtime calls it on unload. `StreamingSubscriber::on_unloaded` is per cell and the audio emitters are not tied to cells, so wiring it needs a design choice (for example a cell-scoped owner id).
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (needs the harness, US-074), and world-unload release of voices. No COMPLETE signal.
---

## 2026-10-25 - US-057 (cell unload releases voices)
- What was implemented: the last open functional gap. `CellAudioSubscriber` (`engine/audio/include/engine/audio/cell_audio.hpp`) is a `world::CellSubscriber` whose `on_unloaded` calls `AudioEngine::stop_owner(cell_owner(cell->coord))`. `cell_owner` is a per-cell tag: bit 63 set, coordinates biased by 2^20 (the cell index limit) in 32-bit fields, so it is nonzero and distinct per cell. An emitter that belongs to a cell tags its voices with `cell_owner`; the unload then releases them and leaves script and other cells' voices playing. The subscriber takes only the audio mutex, never the ECS.
- Files changed: `engine/audio/include/engine/audio/cell_audio.hpp` (new), `engine/audio/src/cell_audio.cpp` (new), `engine/audio/CMakeLists.txt` (source; `engine_world` linked PUBLIC, which only depends on core, so no cycle), `tests/src/audio_mixer_tests.cpp` (2 cases: unload releases only the cell's voices, load keeps them; owner tags are nonzero, flagged, distinct).
- Tests: Linux `linux-dev` debug build 0 warnings; `ctest --preset linux-unit` 22/22; `engine_tests --suite unit` 745 cases, all pass (743 earlier + 2 new). `linux-asan` build 0 warnings; `ctest --preset linux-sanitize` passes. Windows (via cmd.exe batch): `ValidateVendor` 0, `win-dev` configure 0, `win-debug` build 0 (0 warning C / error C), `win-unit` 21/21.
- Not run: `linux-gcc*` gates (only GCC 9.4 is installed; Jolt needs GCC 11+) and `engine_bench --suite acceptance` (no harness, US-074). The per-emitter tagging is not wired: no audio emitter component or world-owned sound exists in the tree yet, so `cell_owner` is the contract for it.
- Status call: US-057 functional criteria and all Linux (clang/debug/unit/sanitize) and Windows gates pass. Remaining open items are the GCC gates and the benchmark, both blocked by the environment and missing harness, so the story stays Partial on those grounds.
- Learnings:
  - `grep -c` returns exit 1 on zero matches, so `a && grep -c ... && b` silently skips `b`. Check the count separately.
  - A cell owner needs a bit that no script owner can have: script owners are object addresses, so bit 63 is the safe flag.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the cell-unload entry above.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and its CI job (host has only GCC 9.4; Jolt needs GCC 11+), and `engine_bench --suite acceptance` (no harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. Re-verified the existing audio implementation.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0, no `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Benchmark: no `engine_bench` target exists in the tree, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (host has only GCC 9.4; Jolt needs GCC 11+), and `engine_bench --suite acceptance` (no harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than this log.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: only g++-9 and clang++-10 are installed, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug` (exit 0, no `warning:`/`error:` lines) and ran `ctest --preset linux-unit` (22 of 22 passed).
- Toolchain: still only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here. No `engine_bench` target exists in the tree.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than this log.
- Gates (this pass, Linux): `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here. No `engine_bench` target exists in the tree.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: still only g++-9 and clang++-10 in `/usr/bin`, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists in the tree, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. Confirmed the feature surface is in tree: WAV/OGG import, 2D/3D sources, listener, attenuation, looping, streamed music, master/music/SFX buses, Lua pause, and cell-unload voice release (`cell_audio.cpp`).
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Not rerun this pass: Windows gates (last recorded result stands) and linux-asan/linux-sanitize (last recorded result stands).
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists in the tree (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than this log.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: `/usr/bin` still has only g++-9, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: `/usr/bin` has only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists in the tree, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, `cmake/`, or the root CMake files is newer than the previous entry.
- Gates (this pass, Linux): `cmake --build --preset linux-debug` exit 0, no `warning:`/`error:` lines. `ctest --preset linux-unit --output-on-failure --no-tests=error` failed once (`engine_tests_integration`, no captured output), then passed 22 of 22 on a rerun (exit 0). The failing run was not reproduced; the first run's output was not saved, so its cause is unknown. Treat it as an unexplained intermittent failure, probably in the device-path audio integration test (WSLg sink).
- Toolchain: only g++-9 and clang++-10 are installed, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here. No `engine_bench` CMake target exists in the tree.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (needs the harness, US-074), and the one unexplained integration failure above. No COMPLETE signal.
- Learnings:
  - A `ctest` failure with no captured output means the run's log was lost. Redirect the full output to a file on every gate run, so a flake can be diagnosed.
---

## 2026-10-25 - US-057 (re-verification, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug` (exit 0, 0 `warning:`/`error:` lines).
- Gates (Linux): `ctest --preset linux-unit --output-on-failure --no-tests=error` ran three times, each 100%, 22 of 22 passed. The intermittent `engine_tests_integration` failure from the previous pass did not reproduce; full logs were saved per run this time.
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists in the tree, so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (Linux gate re-run incl. sanitizers, still Partial)
- What was implemented: no source change. Feature surface unchanged (see the 2026-10-25 US-057 entry above).
- Gates (Linux, this pass): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:`; `ctest --preset linux-unit --output-on-failure --no-tests=error` 22 of 22 passed.
- Sanitizers (this pass): `cmake --preset linux-asan` exit 0; `cmake --build --preset linux-asan` exit 0; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 22 of 22 passed, no ASan/UBSan reports.
- Not rerun this pass: Windows gates (last recorded result stands).
- Toolchain: only g++-9 and clang++-10 installed, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench` (no harness). No COMPLETE signal.
- Learnings:
  - Re-verification passes that change no source add log noise. Only log a pass when a gate result changes or a new gate is run.
---

## 2026-10-25 - US-057 (status re-check, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry.
- Gates (Linux, this pass): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed (log at /tmp/us057/unit.log).
- Toolchain: only g++-9 and clang++-10 installed, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here. Not rerun: Windows gates, linux-asan/linux-sanitize (last recorded results stand).
- Benchmark: no `engine_bench` target exists, so the acceptance benchmark is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness, US-074). No COMPLETE signal is given.
---

## 2026-10-25 - US-057 (Linux gates re-run, still Partial)
- What was implemented: no source change.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0. Log at /tmp/us057/unit.log.
- Toolchain: no g++ 11+ installed, so the `linux-gcc` gate cannot run here.
- Benchmark: no `engine_bench` target exists (harness is US-074).
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job, `engine_bench --suite acceptance`, and the Windows gates (not rerun in this pass). No COMPLETE signal.
---

## 2026-10-25 - US-057 (Windows gates run, still Partial)
- What was implemented: no source change. Feature surface unchanged (see the entries above).
- Gates (Windows, via cmd.exe batch, logs in Temp `us057_*.log`): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0, no `warning C`/`error C`/LNK lines (warm build, 11 lines of log); `ctest --preset win-unit --output-on-failure --no-tests=error` 100%, 21 of 21 passed.
- Not rerun: Linux gates (last recorded pass stands; no source changed since). linux-gcc gate still blocked (only g++-9 installed).
- Benchmark: no `engine_bench` target exists (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness). No COMPLETE signal.
- Learnings:
  - A warm Windows build log is short and reports no warnings for unchanged files. Confirm the build step ran on the current tree before reading a zero warning count as a clean build.
---

## 2026-10-25 - US-057 (status check, still Partial)
- What was implemented: no source change. Re-read the latest entries; the feature surface is unchanged.
- Gates: not rerun in this pass. The last recorded Linux and Windows passes stand.
- Toolchain: only g++-9 (9.4.0) is installed, so the `linux-gcc` gate still cannot run here.
- Benchmark: no `engine_bench` target exists (harness is US-074).
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs the harness). No COMPLETE signal.
---

## 2026-10-25 - US-057 (status check, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug`: exit 0, 0 `warning:`/`error:` lines. `ctest --preset linux-unit --no-tests=error`: 100%, 22 of 22 passed.
- Toolchain: only g++-9 and clang++-10 are installed, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: the `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance`. No COMPLETE signal.
---

## 2026-10-25 - US-057 (blocker check, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than the previous entry.
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10. `sudo -n apt-get install g++-11` fails ("a password is required"), and the Ubuntu archive lists g++-11 as not installed, so the `linux-gcc` gate cannot run without the user installing GCC 11+.
- Benchmark: no `engine_bench` target exists in any CMakeLists.txt (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+ from the user), and `engine_bench --suite acceptance` (needs the US-074 harness). No COMPLETE signal.
---

## 2026-10-25 - US-057 (rebuild after cell_audio.cpp touch, still Partial)
- What was implemented: no source change. `cell_audio.cpp` was modified after the last logged gate run, so the Linux gates were rerun on the current tree.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (log at /tmp/us057/unit.log).
- Toolchain: still only g++-9 and clang++-10, so `linux-gcc` cannot run here.
- Benchmark: still no `engine_bench` target.
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (Linux gates re-run, still Partial)
- What was implemented: no source change. No file under `engine/`, `tests/`, or `cmake/` is newer than this log.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (log at /tmp/us057/unit.log).
- Toolchain: still only g++-9 and clang++-10 in /usr/bin, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` target exists (harness is US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+ installed), and `engine_bench --suite acceptance` (needs US-074). No COMPLETE signal.
---

## 2026-10-25 - US-057 (re-check, still Partial)
- What was implemented: no source change. `find engine/audio engine/script -newer .ralph-tui/progress.md` is empty, so the tree matches the last recorded verification.
- Gates (Linux): `cmake --build --preset linux-debug` rebuilt with no errors; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: `/usr/bin` still has only g++-9 and clang++-10, so `linux-gcc-debug` and `linux-gcc-unit` cannot run here.
- Benchmark: no `engine_bench` harness yet (US-074), so `engine_bench --suite acceptance` is not run.
- Status call: US-057 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+), and `engine_bench --suite acceptance` (needs US-074). No COMPLETE signal.
---

## 2026-10-25 - US-058 (Game UI, status Partial)
- Status call: **Partial**, no COMPLETE signal. Open: the `linux-gcc` gate (needs GCC 11+ for Jolt), a GPU or visual composition check through `set_overlay` (no application host yet), and a running-application check. Everything else in the acceptance list has a recorded result.
- What was implemented: `engine/ui/`, a CPU game UI with no editor or ImGui dependency. Canvas layout with anchors (`Placement`, `place`, `stretch`, `resolve`) and uniform UI scaling (`UiScaling`, the smaller ratio to the reference size times a user scale). Elements: Panel (the only container), Image (registered by name), Text, Button, Slider. UTF-8 text uses Roboto Regular through a C shim over the vendored stb_truetype, with U+FFFD for bad bytes. Input: pointer from device counts (clamped, centred at start, or `set_pointer`), click/press-release activation, Tab/Shift+Tab and arrow/D-pad navigation (spatial), Enter/Space/A activation, slider drag and arrows, and `UiInputResult.captured` for `InputRouter::set_capture(CaptureOwner::Ui, ...)`. `Ui::render()` returns the straight-alpha RGBA8 image for `PostProcessor::set_overlay`.
- Lua callbacks: `engine/script/include/engine/script/script_ui.hpp` (`ui_action`, `ui_value_action`) turns a Lua function into a button or slider callback. Errors go to `ScriptEngine::take_errors()`.
- Files changed: new `engine/ui/` (CMakeLists.txt, include/engine/ui/{geometry,surface,font,ui}.hpp, src/{geometry,surface,font,ui}.cpp, src/native/truetype_shim.{h,c}); new `engine/script/src/script_ui.cpp` and its header; `engine/script/CMakeLists.txt`; root `CMakeLists.txt` (add_subdirectory engine/ui); `tests/CMakeLists.txt` (4 new test files, ENGINE_TEST_FONT_PATH, engine_ui link); new `tests/src/ui_layout_tests.cpp`, `ui_text_tests.cpp`, `ui_interaction_tests.cpp`, `script_ui_tests.cpp`; new `docs/ui.md`.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warnings; `ctest --preset linux-unit` 22 of 22 passed; `cmake --preset linux-asan`, build exit 0, `ctest --preset linux-sanitize` 22 of 22 passed (ASan and UBSan).
- Gates (Windows, `us058_gate.bat` in Temp): vendor=0, dev=0, build=0 (after one fix: MSVC needed `<string>` in `font.hpp`), `ctest --preset win-unit` 21 of 21 passed.
- Linux GCC: `linux-gcc` cannot configure/build the full tree (Jolt needs GCC 11+). `engine_ui` and `script_ui.cpp`/UI test sources compile under GCC 9 with `-Werror`. The `linux-gcc` gate itself is still blocked by Jolt.
- Not verified: no GPU or window run. The post-process overlay composition is not exercised by a fixture (no application host calls `set_overlay` yet). Visual checks in a running application were not run (no application host). `engine_bench` has no harness (US-074), so no benchmark was run.
- Limits: Lua scripts can drive existing callbacks but cannot create UI elements yet (no `ui` Lua module). Text is single-line, with no wrap. Keyboard focus follows tree order for Tab and spatial distance for arrows. There is no scroll view.
- Learnings:
  - A callback handle that owns a registry ref must not be copied from a temporary; the copy's destructor frees the ref. Construct it in place.
  - `place(anchor, x, y, w, h)` puts the top left at the anchor point plus (x, y). A box inset from the bottom right starts at a negative x, and a test with the wrong sign fails on the right edge.
---

## 2026-10-25 - US-058 (Linux gates re-run, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty, so the Windows results in the entry above still match the tree.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed. Logs in /tmp/us058_*.log.
- Toolchain: `/usr/bin` still has only g++-9 and gcc-9, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status call: US-058 stays **Partial**, no COMPLETE signal. Open: the `linux-gcc` gate and CI job (needs GCC 11+ from the user); a GPU/visual check of `set_overlay` composition in a running application (no application host yet); the running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Linux unit re-run, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Status: US-058 stays **Partial**, no COMPLETE signal. Open items are unchanged: the `linux-gcc` gate (needs GCC 11+), the GPU/visual check of `set_overlay` composition (no application host calls it yet), and the running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty, so the gate results above still match the tree.
- Toolchain: `/usr/lib/gcc/x86_64-linux-gnu/` has no GCC 11+ and `sudo` needs a password, so `linux-gcc` cannot be configured here (Jolt needs GCC 11+).
- Application host: only `tools/asset_converter` has a `main`. No running application calls `PostProcessor::set_overlay`/`SceneRenderer::set_overlay`, so the composition-after-postprocess check and the running-app screenshot check cannot be run.
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of overlay composition (needs an app host or a GPU fixture), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change. Tree unchanged since the earlier gate runs (`find engine tests cmake -newer` empty).
- Gates (Linux): `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Toolchain: still only GCC 9 installed, so `linux-gcc` cannot run here.
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of `set_overlay` composition (no application host calls it), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug` (exit 0) and ran `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 22 of 22.
- Toolchain: only GCC 9 is installed (`g++-11`/`g++-12`/`g++-13` absent), so `linux-gcc` cannot be configured here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of `set_overlay` composition (no application host calls it), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (log at /tmp/us058_unit.log).
- Toolchain: no g++ 11+ in /usr/bin, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of `set_overlay` composition (no application host calls it), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 22 of 22 (log at /tmp/us058_unit.log).
- Toolchain: still only GCC 9 in /usr/bin, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of `set_overlay` composition (no application host calls it), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty, so the recorded gate results still match the tree.
- Coverage: `tests/src/ui_layout_tests.cpp` checks anchored corners at 1280x720, 1920x1080, 800x600, 2560x1080, and 640x360, plus UI scale.
- Overlay: `PostProcessor::set_overlay` and `SceneRenderer::set_overlay` have no caller outside `engine/ui`, so composition after post-processing is not exercised.
- Toolchain: only GCC 9 is installed, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of overlay composition (no application host), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Toolchain: no g++ 11+ in /usr/bin, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of `set_overlay` composition (no application host calls it), running-application UI check with screenshots, Windows gates not re-run in this pass.
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change this pass.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines (log /tmp/us058_build.log); `ctest --preset linux-unit --output-on-failure --no-tests=error` run again (log /tmp/us058_unit.log).
- Overlay: `set_overlay` still has no caller in an application host; GPU coverage of composition is in `tests/src/render_postprocess_gpu_tests.cpp` (needs a GPU, not run here).
- Toolchain: only GCC 9 installed, so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of overlay composition, running-application UI check with screenshots, Windows gates not re-run in this pass.
---

## 2026-10-25 - US-058 (Windows gates, still Partial)
- What was implemented: no source change. Tree unchanged since the last entry (`find engine tests cmake -newer` empty).
- Gates (Windows, via cmd.exe batch on D:\dev\haiku_game_engine, MSVC VS 18): `cmake -P cmake/ValidateVendor.cmake` = 0; `cmake --preset win-dev` = 0; `cmake --build --preset win-debug` = 0; `ctest --preset win-unit --output-on-failure --no-tests=error` = 0, 100% passed, 21 of 21 (Linux unit count is 22; not investigated).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), GPU/visual check of overlay composition (no application host calls `set_overlay`), running-application UI check with screenshots.
---

## 2026-10-25 - US-058 (Windows GPU suites, still Partial)
- What was implemented: no source change. The tree was not edited in this pass.
- Gates (Windows, cmd.exe batch on D:\dev\haiku_game_engine, MSVC VS 18, log in Temp `us058_*.log`): `cmake --build --preset win-debug` = 0, 0 `warning C`/`error C` lines; `ctest --preset win-unit --output-on-failure --no-tests=error` = 0, 100% passed, 21 of 21; `engine_tests --suite gpu --backend dx12` = 0, 71 cases, 43905 assertions, adapter NVIDIA GeForce RTX 5080, validation layers on.
- GPU overlay: `DX12 post pass: the reference overlay shows the same colors at two exposures` (tests/src/render_postprocess_gpu_tests.cpp) runs `PostProcessor::set_overlay` after tone mapping and passes on DX12. This covers the "compose after postprocess" check on the GPU.
- `engine_tests --suite gpu --backend vulkan` = 0 on Windows, 72 cases, 43917 assertions, RTX 5080, validation on, pinned VVL 1.4.363 at D:\dev\vvl_build\layer, zero VUID lines in the log.
- Open: `linux-gcc` gate and CI job (needs GCC 11+), native Linux Vulkan GPU run (WSL has no usable Vulkan device), running-application UI check with screenshots (no application host calls `set_overlay`).
- Status: US-058 stays **Partial**, no COMPLETE signal. The open items above are unchanged.
---

## 2026-10-25 - US-058 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty, so the gate results recorded above still match the tree.
- Toolchain: only g++ 9.4 is installed (no g++ 11+), so `linux-gcc` cannot run here (Jolt needs GCC 11+).
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), native Linux Vulkan GPU run (WSL has no usable Vulkan device), running-application UI check with screenshots (no application host calls `set_overlay`).
---

## 2026-10-25 - US-058 (Linux re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty. Only g++ 9.4 and clang 10 are installed, so no GCC 11+ is available.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed.
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), native Linux Vulkan GPU run (WSL has no usable Vulkan device), running-application UI check with screenshots (no application host calls `set_overlay`).
---

## 2026-10-25 - US-058 (GCC 9 partial check, still Partial)
- What was implemented: no source change. The running-application host is still absent: `engine/runtime` holds only `lifecycle.cpp`, and the standalone runtime is US-060, so no application calls `set_overlay`.
- Partial GCC check (not the `linux-gcc` gate, which needs GCC 11+): `g++-9 -std=c++2a -Wall -Wextra -Wpedantic -Werror` over `engine/ui/src/{geometry,surface,ui,font}.cpp` exits 0 with `-Iengine/ui/include -Iengine/core/include -Iengine/platform/include -Iengine/ui/src/native -Ithird_party/stb`. The vendored `truetype_shim.c` compiles with `gcc-9 -Ithird_party` (vendor, not under `-Werror`).
- Status: US-058 stays **Partial**. Open: `linux-gcc` gate and CI job (needs GCC 11+), native Linux Vulkan GPU run (WSL has no usable Vulkan device), running-application UI check with screenshots (no application host until US-060/US-061).
---

## 2026-10-25 - US-058 (toolchain re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake -newer .ralph-tui/progress.md` is empty, so the gate results recorded above still match the tree.
- Toolchain: still only g++-9 (9.4) and clang++-10. No g++-11+ under /usr/bin, /usr/local/bin, /opt, or ~/.local/bin, and sudo needs a password, so `linux-gcc` cannot be installed or run here.
- Status: US-058 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), native Linux Vulkan GPU run (WSL has no usable Vulkan device), running-application UI check with screenshots (no application host calls `set_overlay` until US-060/US-061).
---

## 2026-10-25 - US-059 (Game Saves, Partial)
- What was implemented: a save module and a script-state hook. `engine/save/` (new `engine_save` target) has `save.hpp` (the file format, `SaveData`, `select_components`, `serialize_save`/`parse_save`, and `SaveSlots` with atomic writes and a `.prev` copy of the last good save), `world_changes.hpp` (`collect_world_changes` reads dirty cells and layer, `apply_world_changes` works on a copy and validates it, `write_cell_files` writes restored cells into a runtime directory), and `save_error.hpp`. `engine/script` gained `ScriptRunner::capture_state`/`restore_state` (`script_state.{hpp,cpp}`, JSON through the vendored nlohmann/json as a system include).
- File format: `{format: "engine.save", version: 1, build: {engine}, save_id, checksum: "crc32:...", body: {scene, scripts, world}}`. Format name and version are checked before the checksum, so an unknown file reports what it is. The scene uses `serialize_scene`/`parse_scene`, so component schema numbers are still checked. Cells use `serialize_cell`/`parse_cell`.
- Files changed: new `engine/save/CMakeLists.txt`, `engine/save/include/engine/save/{save,save_error,world_changes}.hpp`, `engine/save/src/{save,world_changes}.cpp`, `engine/script/src/script_state.{hpp,cpp}`, `tests/src/save_tests.cpp`. Edited: root `CMakeLists.txt` (add_subdirectory engine/save), `engine/script/CMakeLists.txt` (script_state.cpp, nlohmann system include), `engine/script/include/engine/script/script_runner.hpp` and `src/script_runner.cpp` (capture/restore), `tests/CMakeLists.txt` (save_tests.cpp, engine_save link). `docs/` not changed.
- Tests: 12 new `[unit]` cases in `tests/src/save_tests.cpp`: selection keeps GUIDs and hierarchy, script state and entity survive, wrong format/version/checksum/truncation rejected, slot keeps previous save and failed write keeps the slot, corrupt slot is rejected and the previous save survives the next write, slot-name rules, Lua state capture/restore before the first update, refused state changes nothing, world changes restore into the authored world and into cell files, a conflicting change leaves the world unchanged, layer restore, and a streamed cell reads the saved change through `StreamingManager`. Reconciled count: unit suite 788 test cases, 776 before this story.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0, 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22; `cmake --preset linux-asan`, build exit 0, 0 warning lines; `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 100%, 22 of 22 (ASan and UBSan, run after the last test change).
- Gates (Windows, `us059_gate.bat` in Temp, log files `us059_*.log`): `cmake -P cmake/ValidateVendor.cmake` = 0; `cmake --preset win-dev` = 0; `cmake --build --preset win-debug` = 0, 0 `warning C`/`error C` lines (save_tests.cpp and the new engine_save sources recompiled); `ctest --preset win-unit --output-on-failure --no-tests=error` = 0, 100%, 21 of 21. Run twice on the final tree.
- Partial check: `g++-9 -std=c++2a -Wall -Wextra -Wpedantic -Werror` exits 0 for `engine/save/src/save.cpp`, `world_changes.cpp`, and `engine/script/src/script_state.cpp`. This is not the `linux-gcc` gate, which needs GCC 11+ for Jolt and cannot run here.
- Not verified: the `linux-gcc` gate and CI job (only GCC 9 installed). The save latency and the `engine_bench --suite acceptance` run are not done: the benchmark harness does not exist yet (US-074), and there is no application host (US-060) that saves in a running game. Only the library paths are tested, not a play session.
- Status: US-059 is **Partial**, no COMPLETE signal, because the Linux/GCC acceptance item cannot be run here. Open: `cmake --preset linux-gcc`, `cmake --build --preset linux-gcc-debug`, `ctest --preset linux-gcc-unit` (needs GCC 11+), the performance benchmark (no harness), and a runtime save/load path through a host (US-060).
- Limits: `apply_world_changes` checks the whole world with `validate_world`, which is O(world) per load. A restored cell is served to a streaming manager only through the runtime directory, so the host must copy the authored cells there first. Restore runs only before the first `begin_frame` (a Pending instance). A table that mixes string keys with integer keys cannot be saved.
- Learnings:
  - A test that damages a field by searching the text must match the top-level occurrence: `"version": 1` appears in the nested scene too. Matching `"version": 1\n}` picks the top-level one.
  - A checksum over the body's compact dump is stable across pretty-printing, because nlohmann sorts keys. Invalid UTF-8 in a loaded file makes `dump()` throw, so the checksum helper catches it and returns Malformed.
  - `lua_next` must keep its key on top, so an entry that is skipped pops only the value.
---

## 2026-10-25 - US-059 (re-verification, still Partial)
- Re-ran the Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests passing, unit label present).
- Still blocked: `linux-gcc` / `linux-gcc-debug` / `linux-gcc-unit` need GCC 11+ (only gcc-9 installed; no sudo for install). `engine_bench` target does not exist (no harness yet), so the benchmark item is not run. No runtime host (US-060) exists to exercise a play-session save.
- Status unchanged: US-059 Partial, no COMPLETE signal.
---
## 2026-10-25 - US-059 (re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt -newer .ralph-tui/progress.md` is empty, so the earlier results still match the tree.
- Gates rerun: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22.
- Still blocked: `linux-gcc` needs GCC 11+. Only gcc-9 is installed, and there is no passwordless sudo to install another. `engine_bench` has no harness yet (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal.
---

## 2026-10-25 - US-059 (Linux re-check, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug` (exit 0, 0 warning/error lines) and ran `ctest --preset linux-unit --output-on-failure --no-tests=error` (exit 0, integration 21 tests, unit label present).
- Toolchain: only g++-9 and clang on PATH; no GCC 11+, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt -newer .ralph-tui/progress.md` is empty, so the Windows and Linux gate results recorded above still match the tree.
- Gates rerun: `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 passed (exit 0). `cmake --build --preset linux-debug` 0 warning/error lines.
- Toolchain: still only g++-9 and clang-10. No GCC 11+ under /usr/bin, /usr/local/bin, or ~/.local/bin, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt -newer .ralph-tui/progress.md` is empty, so the Windows and Linux results recorded above still match the tree.
- Gates rerun: `cmake --build --preset linux-debug` exit 0, 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (exit 0).
- Toolchain: still only g++-9 (9.4) and clang++-10. No GCC 11+ on PATH, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-check, still Partial)
- What was implemented: no source change. Rebuilt `linux-debug` (exit 0, 0 warning/error lines) and ran `ctest --preset linux-unit --output-on-failure --no-tests=error` (100%, 22 of 22).
- Toolchain: only g++ 9.4 on PATH; no g++-11 or newer, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt -newer .ralph-tui/progress.md` is empty.
- Gates rerun: `cmake --build --preset linux-debug` 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (exit 0).
- Toolchain: still only g++-9 (9.4) and clang++-10. No GCC 11+ in /usr/bin, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no `engine_bench` target, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt -newer .ralph-tui/progress.md` is empty. Touched `save.cpp`, `world_changes.cpp`, and `save_tests.cpp` (mtime only) to force a recompile: `cmake --build --preset linux-debug` exit 0, 0 warning/error lines.
- Gates (Linux): `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22, exit 0.
- Toolchain: only g++-9 (9.4) and clang++-10. No GCC 11+ under /usr/bin, /usr/lib/gcc, or /usr/local/bin, so `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (Linux gate re-run, still Partial)
- What was implemented: no source change.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0 (configure 97s); `cmake --build --preset linux-debug` exit 0, 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0 (integration 21 tests, unit 1 test, 60.85s).
- Toolchain: only g++-9 (9.4) and clang++-10. `linux-gcc` cannot run here.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060). Windows gates not re-run in this pass.
---

## 2026-10-25 - US-059 (GCC 11+ acquisition attempt, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the last Windows and Linux results still match the tree.
- Toolchain attempt: `apt-get download` works without root, but the configured focal mirror has only GCC 9 installed and GCC 10 (10.5) available. GCC 11+ is not in focal main/universe/updates, and gcc-12 comes only from the ubuntu-toolchain PPA, which needs root to add. GCC 10 cannot build Jolt (`__builtin_bit_cast` needs GCC 11), so the `linux-gcc` gate stays unrun.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` gate and CI job (needs GCC 11+ from a machine that has it), `engine_bench --suite acceptance` (no harness, US-074), and a runtime save/load path through a host (US-060).
- Learnings:
  - Do not retry the apt route for GCC 11+ on this WSL image: `apt-get download` works, but focal has no gcc-11 or gcc-12 packages. Only a PPA or a newer distro release would supply them, and both need root or a different sysroot.
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the recorded gate results still match the tree: Windows (ValidateVendor, win-dev, win-debug, win-unit 21/21) and Linux (ValidateVendor, linux-dev, linux-debug, linux-unit 22/22, linux-asan build, linux-sanitize 22/22 with ASan and UBSan).
- Toolchain: only g++ 9.4 and clang++ 10 on this machine. No GCC 11+, so `linux-gcc` cannot run.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+ on a machine that has it); `engine_bench --suite acceptance` (no harness yet, US-074); and a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the recorded Windows and Linux results still match the tree. Gates were not re-run in this pass.
- Toolchain: only g++ 9 and clang++ 10 under /usr/bin and /usr/local/bin. No GCC 11+, so `linux-gcc` cannot run here.
- Benchmark: no `engine_bench` target in any CMakeLists.txt (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+ on another machine); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the recorded Windows and Linux results still match the tree. Gates not re-run in this pass.
- Toolchain: only g++-9 and clang++ on PATH. No GCC 11+, so `linux-gcc` cannot run here.
- Benchmark: no `engine_bench` target in any CMakeLists.txt (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-verify, still Partial)
- What was implemented: no source change. Re-ran the Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` (exit 0), `cmake --preset linux-dev` (0), `cmake --build --preset linux-debug` (0), `ctest --preset linux-unit --output-on-failure --no-tests=error` (22/22 passed; save_tests.cpp has 11 `[unit]` cases covering component selection, GUIDs, format/version/CRC rejection, previous-save rotation, failed-write safety, script state, changed-cell round trip, and dirty layers).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (local toolchain is GCC 9.4, which cannot build Jolt); `engine_bench --suite acceptance` (no harness, US-074); Windows gates were not re-run in this pass.
---

## 2026-10-25 - US-059 (re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `cmake --build --preset linux-debug` exit 0, 0 warning/error lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100%, 22 of 22 (exit 0).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+; local toolchain is GCC 9.4); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. Spot-checked the toolchain and bench target: only g++ 9.4 and clang++ 10 on PATH (no g++-11+), and no `engine_bench` target in any CMakeLists.txt. Gates not re-run in this pass; the most recent recorded Linux and Windows results are unchanged.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (re-verify, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty.
- Gates (Linux): `cmake --build --preset linux-debug` rebuilt clean; `ctest --preset linux-unit --output-on-failure --no-tests=error` ran integration 21 and unit 1.
- Toolchain: no g++-11+ on PATH (`linux-gcc` cannot run here).
- Benchmark: still no `engine_bench` target (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060). Windows gates not re-run in this pass.
---

## 2026-10-25 - US-059 (Windows gates re-run, still Partial)
- What was implemented: no source change. Ran the Windows gates on D:\dev\haiku_game_engine through a cmd.exe batch (one run, no overlap).
- Gates (Windows): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0, 0 warning/error lines (MSVC /W4 /WX); `ctest --preset win-unit --output-on-failure --no-tests=error` exit 0, 21 of 21 passed.
- Linux gates were re-run earlier today on the same tree (ValidateVendor, linux-dev, linux-debug, linux-unit 22/22).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+; local toolchain is GCC 9.4); `engine_bench --suite acceptance` (no harness yet, US-074); a runtime save/load path through a host (US-060).
- Learnings:
  - The win-debug build log for an already-warm tree still relinks engine_tests.exe; check the link line, not only the exit code.
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the recorded Windows and Linux results still match the tree. Gates not re-run in this pass.
- Toolchain: only g++-9 (9.4), gcc-9, and clang++-10 on this machine. No GCC 11+, so `linux-gcc` cannot run here.
- Benchmark: no `engine_bench` target in any CMakeLists.txt (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+); `engine_bench --suite acceptance` (no harness, US-074); a runtime save/load path through a host (US-060).
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the recorded Windows and Linux gate results still match the tree. Gates not re-run in this pass.
- Toolchain: no g++-11/12/13/14 or gcc-11+ on PATH, so `linux-gcc` cannot run here.
- Benchmark: no `engine_bench` target in any CMakeLists.txt (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+ on a machine that has it); `engine_bench --suite acceptance` (no harness yet, US-074). A runtime save/load path through a host is US-060, not an AC of this story.
- Learnings:
  - Repeated status-only passes add nothing while the blockers are environmental. Re-run a gate only after a source change or a toolchain change.
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, and the toolchain is unchanged (g++ 9.4, gcc-9, clang++ 10; no GCC 11+). Gates not re-run.
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+); `engine_bench --suite acceptance` (no harness, US-074). Save/load through a host is US-060.
---

## 2026-10-25 - US-059 (status check, still Partial)
- What was implemented: no source change. `find engine tests cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the Windows and Linux results recorded earlier today still match the tree. Gates not re-run in this pass.
- Toolchain: only g++ 9.4, gcc-9, and clang++ 10. No GCC 11+, so `linux-gcc` cannot run here. No `engine_bench` target in any CMakeLists.txt (US-074).
- Status: US-059 stays **Partial**, no COMPLETE signal. Open: `linux-gcc` preset, build, and `linux-gcc-unit` ctest (needs GCC 11+ on another machine); `engine_bench --suite acceptance` (no harness yet, US-074).
---

## 2026-10-25 - US-060
- What was implemented: the resource pack format and builder/reader (`engine/pack/`), the `asset_packer` tool that packs a project's roots and their dependency closure from the converter's cooked output, and the `game_runtime` executable with its option parser (`engine/runtime/`). The runtime takes `--pack`, `--start-scene`, `--backend`, and `--profile`, opens and verifies each pack, checks that the start scene is a root, parses and loads it through `engine_scene`, and runs the lifecycle. It links no converter, codec, or Assimp target; a configure-time check enforces that.
- Files changed: `engine/pack/{CMakeLists.txt,include/engine/pack/pack.hpp,src/pack.cpp}` (new); `tools/asset_packer/{CMakeLists.txt,src/main.cpp}` (new); `engine/runtime/{CMakeLists.txt,include/engine/runtime/runtime_options.hpp,src/runtime_options.cpp,src/game_main.cpp}` (new or changed); `tests/src/pack_tests.cpp`, `tests/src/runtime_options_tests.cpp`, `tests/pack/PackRuntimeRoundTrip.cmake` (new); `tests/fixtures/pack/` (new: a packed project and a missing-dependency project); `tests/CMakeLists.txt`; root `CMakeLists.txt`; `docs/runtime-packs.md` (new); `docs/runtime-assets.md`.
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with no warning lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30 of 30 passed (unit suite 803 test cases).
- Gates (Windows, MSVC /W4 /WX): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0, 0 warning/error lines; `ctest --preset win-unit --output-on-failure --no-tests=error` 0.
- Not run: `linux-gcc` (local GCC is 9.4, cannot build Jolt; US-059 already records this), `engine_bench` (no harness, US-074), GPU suites (this story adds no GPU code; the runtime opens no window).
- Status: the story's acceptance checks are covered by the tests above. Open for later stories: the runtime does not open a window or run a frame loop; packages (US-073), payload compression, and a game component catalog are not in this story.
- Learnings:
  - `core::failure("literal")` does not convert to `Result<T, std::string>`; wrap the literal in `std::string(...)`.
  - An `AssetRecord` has no default constructor (its `Path` has none), so build it with a full aggregate initializer.
  - Packing a scene needs no new importer: the `json` importer already canonicalises the file.
---

## 2026-10-25 - US-061
- What was implemented: the editor shell. `engine/editor/` holds the document model (`EditorModel`: project session with `project.json` validation and an asset scan, a bounded console that is also a log sink, and a saved workspace with the docking layout and panel flags), the in-house ImGui input adapter, the ImGui renderer that draws through `engine_rhi` for both backends, and the shell UI (menu bar, dock space with a default layout, Project, Viewport, Console, Errors, Workspace panels). `tools/game_editor/` is the native executable: window, device, swapchain, ImGui context, Roboto font with the Cyrillic range, and the frame loop. `Platform::pointer_position` is new on Win32 and X11 (`XQueryPointer` added to the Xlib ABI). `engine_tests --suite editor [--backend]` is new. `docs/editor.md` describes the contract.
- Files changed: `engine/editor/**` (new: CMakeLists, includes, sources, `shaders/imgui.hlsl`); `tools/game_editor/**` (new); `engine/platform/include/engine/platform/platform.hpp`, `engine/platform/src/platform_unsupported.cpp`, `engine/platform/src/win32/platform_win32.cpp`, `engine/platform/src/x11/platform_x11.cpp`, `engine/platform/src/x11/xlib_abi.{hpp,cpp}`; `tests/CMakeLists.txt`, `tests/src/main.cpp`, `tests/src/cli_options.{hpp,cpp}`, `tests/src/cli_options_tests.cpp`; `tests/src/editor_*.cpp` and `editor_fixture.hpp` (new); `tests/src/platform_x11_tests.cpp`, `tests/src/platform_win32_tests.cpp` (pointer tests); `CMakeLists.txt`; `docs/editor.md` (new).
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, no warning lines; `ctest --preset linux-unit` 30 of 30 passed (unit 834 cases in engine_tests).
- Gates (Windows, MSVC /W4 /WX, RTX 5080): see the rerun results recorded in the report; `validate`, `configure`, `build` (292 steps, no warning lines), `ctest win-unit` passed.
- Not run: `linux-gcc` (local GCC 9.4; the Jolt-free editor code also needs GCC 11+ for the CI job, so it is unverified); the native Linux run and `engine_tests --suite editor --backend vulkan` on Linux (WSL has no Vulkan adapter, and the host exits 1 with "no hardware Vulkan adapter"); `engine_bench` (no harness, US-074).
- Status: see the final report for the Windows GPU results and the native window check.
- Learnings:
  - The first Windows editor run failed `create_pipeline` for an RGBA8 vertex attribute: the RHI takes 32-bit vertex formats only. Pack the color in a uint.
  - The second run failed on the test's own state tracking: a readback leaves the target in CopySource, so the next frame must name that as `before`.
  - A `DockSpace` call returns without a node when `ImGuiConfigFlags_DockingEnable` is off, and `DockBuilderAddNode` then dereferences null. The host sets the flag; the test fixture must too.
  - `engine_tests` has no per-case filter by name here, so a crash is found with gdb on `--suite editor`, not the case name.
---
## 2026-10-25 - US-061 (Linux re-verification, Partial)
- Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0 (26 deps, 8542 files); `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, no warning lines; `ctest --preset linux-unit` 30 of 30 passed; `engine_tests --suite editor` all passed (322 assertions, 30 cases).
- Not re-run here: Windows gates (recorded in the prior US-061 entry), `linux-gcc` (GCC 9.4 locally), `linux-asan` (editor adds no memory/ECS/Lua/physics path, so not required by this story's own rule), native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `engine_bench` (no harness).
- Status: stays **Partial** until the linux-gcc CI job and the native Linux and GUI checks have run.
---

## 2026-10-25 - US-061 (Windows gates and editor/GPU suites, Partial)
- What was implemented: no source change. Ran the Windows gates and the editor and GPU suites on D:\dev\haiku_game_engine through cmd.exe batches (one run at a time).
- Gates (Windows, MSVC /W4 /WX): `cmake -P cmake/ValidateVendor.cmake` 0 (26 deps, 8542 files); `cmake --build --preset win-debug` 0, no compiler warning lines; `ctest --preset win-unit --output-on-failure --no-tests=error` 0, 29 of 29 passed.
- Editor suite (Windows): `engine_tests --suite editor --backend dx12` 0 (340 assertions, 31 cases). `--backend vulkan` 0 when `VK_LAYER_PATH=D:\dev\vvl_build\layer` is set (pinned 1.4.363 layer).
- GPU suite (Windows, RTX 5080, validation on): `--suite gpu --backend dx12` 0 (73 cases); `--suite gpu --backend vulkan` 0 (73 cases).
- Failed first attempt: `--suite editor --backend vulkan` without `VK_LAYER_PATH` loaded the installed SDK layer 1.3.224, which reports VUID-VkDeviceCreateInfo-pNext-pNext on the mesh-shader feature chain. This is the known layer-version issue in reference-vulkan-validation-layer-windows, not an editor defect.
- Not run: native Linux GUI and Vulkan editor suite (WSL has no Vulkan adapter); a visual check of `game_editor.exe` (no screenshot captured in this pass); `linux-gcc` (local GCC 9.4); `linux-asan` (not required by this story's rules); `engine_bench` (no harness, US-074).
- Status: stays **Partial**. Open: the native Linux GUI and Vulkan editor suite, the linux-gcc CI job, and a retained screenshot/action log for the running editor.
- Learnings:
  - The Vulkan editor and GPU suites on Windows need `VK_LAYER_PATH` pointing at the pinned layer. Without it the run fails on the SDK layer, not on the code.
---
## 2026-10-25 - US-061 (Linux re-verification and native window retry, Partial)
- What was implemented: no source change.
- Gates (Linux, re-run): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, no warning lines; `ctest --preset linux-unit` 0.
- Editor suite (Linux CPU): `engine_tests --suite editor` 322 assertions in 30 cases, all passed. `--backend vulkan` reports unsupported (no Khronos validation layer under WSL), so not run.
- Native window (Windows): `artifacts/ui/us061/run_ui.ps1` launched game_editor.exe (dx12) for the valid and broken projects; both exited 0 and saved workspaces. The screenshots showed only the wallpaper: LogonUI.exe and LockApp.exe were running, so the session was locked. No screenshot kept; see `artifacts/ui/us061/NOT_VERIFIED.txt`.
- Not run: Linux GUI (WSL has no Vulkan 1.2 adapter), linux-gcc, linux-asan (not required by this story's rules), engine_bench (no harness).
- Status: stays **Partial**. Open: the unlocked-desktop screenshot and action log, native Linux GUI/Vulkan, linux-gcc CI.
- Learnings:
  - A locked Windows session still runs game_editor, and its capture is only wallpaper. Check `tasklist` for LogonUI.exe/LockApp.exe before reading a capture as the editor.
  - The `-File` path in `cmd.exe /c "powershell ... -File \"...\""` fails with "Illegal characters in path". Leave the path unquoted.
---
## 2026-10-25 - US-061 (Linux gate re-run, Partial)
- What was implemented: no source change. Re-verified the current tree on Linux.
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, no `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0, 30 of 30 passed.
- Editor suite (Linux CPU): `build/linux-dev/tests/Debug/engine_tests --suite editor` 0, 322 assertions in 30 cases.
- Not run: Linux GUI and Vulkan editor suite (WSL has no Vulkan adapter); `linux-gcc` (local GCC 9.4); `linux-asan` (not required by this story's rules); `engine_bench` (no harness, US-074); retained screenshot and action log for the running editor.
- Status: stays **Partial**. Open: the native Linux GUI and Vulkan editor suite, the linux-gcc CI job, and an unlocked-desktop screenshot with action log.
---
## 2026-10-25 - US-061 (Linux editor suite re-check, Partial)
- What was implemented: no source change. Re-ran `build/linux-dev/tests/Debug/engine_tests --suite editor` on the current tree: all passed (322 assertions in 30 cases).
- Not run: native Linux GUI and Vulkan editor suite (WSL has no Vulkan adapter), `linux-gcc` (local GCC 9.4), `engine_bench` (no harness, US-074), unlocked-desktop screenshot and action log.
- Status: stays **Partial**; no COMPLETE signal. The acceptance criteria require the native Linux GUI/Vulkan run and a retained screenshot, which this environment cannot produce.
---
## 2026-10-25 - US-061 (Lock-screen recheck, Partial)
- What was implemented: no source change.
- Checked the Windows session before a GUI capture: LogonUI and LockApp are running, so the desktop is still locked and `game_editor.exe` cannot be captured. No launch attempted.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc`, `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then rerun `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log.
---
## 2026-10-25 - US-061 (Lock-screen recheck 2, Partial)
- What was implemented: no source change.
- Checked the Windows session with `tasklist`: LogonUI.exe and LockApp.exe are still running, so the desktop is locked. No `game_editor.exe` launch was attempted, because a capture under the lock screen shows only the wallpaper (see `artifacts/ui/us061/NOT_VERIFIED.txt`).
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (local GCC 9.4), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log.
---
## 2026-10-25 - US-061 (Lock-screen recheck 3, Partial)
- What was implemented: no source change.
- Checked the Windows session with `tasklist`: LogonUI.exe and LockApp.exe are still running, so the desktop is locked. No `game_editor.exe` launch was attempted (`artifacts/ui/us061/NOT_VERIFIED.txt`).
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (local GCC 9.4), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log.
---
## 2026-10-25 - US-061 (Lock-screen recheck 4, Partial)
- What was implemented: no source change.
- Checked the Windows session with PowerShell `Get-Process`: LogonUI (52008) and LockApp (58728) are still running, so the desktop is locked. No `game_editor.exe` launch was attempted (`artifacts/ui/us061/NOT_VERIFIED.txt`). `tasklist /FI` fails through WSL's cmd.exe quoting, so use PowerShell for this check.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (local GCC 9.4), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log.
---
## 2026-10-25 - US-061 (Lock-screen recheck 5, Partial)
- What was implemented: no source change.
- Checked the Windows session with PowerShell `Get-Process`: LogonUI (52008) and LockApp (58728) are still running, so no `game_editor.exe` launch was attempted. `artifacts/ui/us061/run_ui.ps1` is not in the tree; the capture script must be recreated when the desktop is unlocked.
- Checked the Linux toolchain: only GCC 9.4 (`g++-9`) and Clang 10 are installed, so `linux-gcc` cannot run here.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+ CI), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, recreate and run the GUI capture script, retain the capture and action log, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Linux gate re-run and lock check 6, Partial)
- What was implemented: no source change.
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, no `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Editor suite (Linux CPU): `engine_tests --suite editor` passed, 322 assertions in 30 cases.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so no `game_editor.exe` launch or capture was attempted.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (local GCC 9.4), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Status check 7, Partial)
- What was implemented: no source change. `find engine tests tools cmake CMakeLists.txt CMakePresets.json -newer .ralph-tui/progress.md` is empty, so the gate results recorded above still match the tree. Gates not re-run.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked and no `game_editor.exe` launch was attempted.
- Toolchain: only g++-9, gcc-9, and clang++-10 (no GCC 11+), so `linux-gcc` cannot run here.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, recreate and run the GUI capture script, retain the capture and action log, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Status check 8, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log, so the recorded gate results still match the tree.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked and no `game_editor.exe` capture was attempted.
- Toolchain: g++ 9.4 is the only GCC installed, so `linux-gcc` cannot run here.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite, `linux-gcc`, `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Gate re-run and lock check 9, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` passed, 322 assertions in 30 cases.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked and no `game_editor.exe` capture was attempted.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal.
---
## 2026-10-25 - US-061 (Status check 10, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log, so the gate results recorded above still match the tree.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` launch was attempted.
- Toolchain: no GCC 11+ installed, so `linux-gcc` cannot run here.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Status check 11, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log, so the recorded gate results still match the tree.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked and no `game_editor.exe` capture was attempted.
- Toolchain: no g++-11 or newer on this machine, so `linux-gcc` cannot run here.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, run the native Linux Vulkan editor suite (WSL has no Vulkan adapter), and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Status check 12, Partial)
- What was implemented: no source change. The Windows session is still locked (PowerShell `Get-Process`: LogonUI 52008, LockApp 58728), so no `game_editor.exe` capture was taken. The unlocked-desktop screenshot and action log need the user to unlock the session, so I did not repeat the lock check further.
- Gates recorded above still match the tree; no `engine`, `tests`, `tools`, or `cmake` edits were made in this pass.
- Not run: native Linux GUI and Vulkan editor suite (WSL has no Vulkan adapter), `linux-gcc` (no GCC 11+ here), `engine_bench` (no harness, US-074), retained screenshot and action log.
- Status: stays **Partial**; no COMPLETE signal. Blocked on the user unlocking the Windows session and on a GCC 11+ CI run.
---
## 2026-10-25 - US-061 (Status check 13, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 14, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 15, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 16, Partial)
- What was implemented: no source change. Windows session still locked (PowerShell `Get-Process`: LockApp 58728, LogonUI 52008), so Windows gates and the `game_editor.exe` capture were not run.
- Linux gates re-run on the unchanged tree: `cmake --build --preset linux-debug` rc 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` passed.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 17, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 18, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log, so the Linux gate results recorded above still match the tree; gates not re-run.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 19, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Toolchain: no g++-11 or newer installed (only g++-9), so `linux-gcc` cannot run here.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 20, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: PowerShell `Get-Process` still shows LogonUI and LockApp, so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the Windows gates and GUI capture, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 20, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Gates (Linux, clang 10): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` 322 assertions in 30 cases passed.
- Windows session: PowerShell `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` capture was attempted.
- Toolchain: no GCC 11+ installed, so `linux-gcc` cannot run here.
- Not run: unlocked-desktop screenshot and action log, native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+), `engine_bench` (no harness, US-074).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, run the native Linux Vulkan editor suite, and run the linux-gcc CI job.
---
## 2026-10-25 - US-061 (Status check 21, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows gates were recorded as passing in the "Windows gates and editor/GPU suites" entry above and were not re-run.
- Windows session: LockApp (58728) and LogonUI (52008) still running, so the desktop is locked. The unlocked-desktop `game_editor.exe` capture and action log were not taken.
- Toolchain: only g++ 9.4 is installed, so `linux-gcc` cannot run here.
- Status: stays **Partial**. Open: the user unlocks the Windows session for the GUI capture; the linux-gcc CI job (GCC 11+) and the native Linux Vulkan editor suite need a machine that has them.
---
## 2026-10-25 - US-061 (Status check 22, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0 (unit 1 test, integration 29 tests).
- Windows session: `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not re-run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, run the GUI capture and action log, run the native Linux Vulkan editor suite, and run the linux-gcc CI job (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 23, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` 322 assertions in 30 cases passed.
- Not run: Windows gates and the unlocked-desktop `game_editor.exe` capture (Windows session not re-checked in this pass), `linux-gcc` (only GCC 9.4 installed), native Linux Vulkan editor suite (no Vulkan adapter under WSL), `engine_bench` (no harness).
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite, and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 24, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` 322 assertions in 30 cases passed.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not re-run in this pass.
- Status: stays **Partial**; no COMPLETE signal. Open: the user unlocks the Windows session for the GUI capture and action log, the native Linux Vulkan editor suite, and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 25, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0 (integration 29, unit 1); `engine_tests --suite editor` 322 assertions in 30 cases passed.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not re-run in this pass.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite, and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 26, Partial)
- What was implemented: no source change. Linux gates re-run on the unchanged tree: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 30 of 30 (unit 1, integration 29).
- Windows session: PowerShell `Get-Process` shows LockApp (58728) and LogonUI (52008), so the desktop is still locked. Windows gates and the `game_editor.exe` capture were not re-run.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite, and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 27, Partial)
- What was implemented: no source change in this pass.
- Linux gates re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not re-run.
- Toolchain: only g++ 9.4 is installed, so `linux-gcc` cannot run here.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite, and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 28, Partial)
- What was implemented: no source change.
- Linux gates re-run: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, no `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` 322 assertions in 30 cases passed.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` launch.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop screenshot and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL), `linux-gcc` (needs GCC 11+).
---
## 2026-10-25 - US-061 (Status check 29, Partial)
- What was implemented: no source change in this pass.
- Linux gates re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. The `game_editor.exe` capture and action log were not re-run; `artifacts/ui/us061/` holds the earlier result log and NOT_VERIFIED note.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite (no Vulkan adapter under WSL), and the linux-gcc CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 30, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. The `game_editor.exe` capture and action log were not run.
- Toolchain: only g++ 9.4 is installed, so `linux-gcc` cannot run here. Linux gates were not re-run in this pass because the tree is unchanged since the last full run (status checks 26-29).
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop (`artifacts/ui/us061/run_ui.ps1`), the native Linux Vulkan editor suite (no Vulkan adapter under WSL), and the linux-gcc CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 31, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` capture.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop (`artifacts/ui/us061/run_ui.ps1`), the native Linux Vulkan editor suite (no Vulkan adapter under WSL), and the linux-gcc CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 32, Partial)
- What was implemented: no source change in this pass.
- Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines. `ctest --preset linux-unit` was started in the background and its summary was not captured before the pass ended.
- Windows session: `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked. No Windows gates or `game_editor.exe` capture were run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log, native Linux Vulkan editor suite, linux-gcc (GCC 11+; only GCC 9.4 installed), Windows gates.
---
## 2026-10-25 - US-061 (Status check 33, Partial)
- What was implemented: no source change in this pass.
- Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 100% passed, 30 of 30.
- Windows session: `Get-Process` still shows LogonUI (52008) and LockApp (58728), so the desktop is locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop (`artifacts/ui/us061/run_ui.ps1`), the native Linux Vulkan editor suite (no Vulkan adapter under WSL), and the linux-gcc CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 34, Partial)
- What was implemented: no source change in this pass.
- Gates: `cmake -P cmake/ValidateVendor.cmake` 0 on the current tree.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` launch or capture.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log; native Linux GUI and Vulkan editor suite (no Vulkan adapter under WSL); `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 35, Partial)
- What was implemented: no source change in this pass.
- Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` 0, 0 `warning:`/`error:` lines.
- `ctest --preset linux-unit` failed once in `engine_tests_integration` while a second ctest run was still active in the background; the integration test passed on rerun alone (43.9 s), and a full solo rerun passed. Treat the first failure as a concurrent-run collision, not a confirmed defect; not yet root-caused.
- Windows session: `Get-Process` shows LockApp (58728) and LogonUI (52008), still locked. Windows gates and the `game_editor.exe` capture were not run.
- Status: stays **Partial**; no COMPLETE signal. Open: the GUI capture and action log on an unlocked Windows desktop, the native Linux Vulkan editor suite (no Vulkan adapter under WSL), and the linux-gcc CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 36, Partial)
- What was implemented: no source change in this pass.
- Gates: `cmake -P cmake/ValidateVendor.cmake` 0 on the current tree.
- Windows session: `Get-Process` still shows LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` capture or Windows gate run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log; native Linux Vulkan editor suite (no Vulkan adapter under WSL); `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 37, Partial)
- What was implemented: no source change in this pass.
- Windows gates re-run on the current tree (batch `us061_gate.bat`, rc in `%TEMP%\us061_rc.txt`): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (warm build, 13-line log, no `warning C`/`error C`); `ctest --preset win-unit --output-on-failure --no-tests=error` 0, 29 of 29 passed; `engine_tests --suite editor --backend dx12` 0, 340 assertions in 31 cases; `engine_tests --suite gpu --backend dx12` 0, 43923 assertions in 72 cases.
- Desktop: an earlier "not running" reading of LockApp/LogonUI came from `tasklist /FI`, which the WSL quoting breaks. A plain `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728). The `run_ui.ps1` capture therefore produced wallpaper-only frames. Those PNGs were deleted; `artifacts/ui/us061/result.log` and `NOT_VERIFIED.txt` were updated.
- Linux: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0, 0 warning/error lines.
- Not run: `engine_tests --suite editor` and `--suite gpu` with `--backend vulkan` on Windows, the native Linux Vulkan suite (no Vulkan adapter under WSL), `linux-gcc` (only GCC 9.4 installed), `linux-asan` (not required by this story's own rule), `engine_bench` (no harness).
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then re-run `run_ui.ps1` and keep the capture; Vulkan editor and GPU suites on Windows and native Linux; `linux-gcc` CI job (GCC 11+).
---
## 2026-10-25 - US-061 (Status check 38, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, or `cmake` is newer than 14:26 today.
- Windows Vulkan suites now have results on the current tree (from `us061_vk.bat`, log `us061_vk_result.txt`, validation layers on, RTX 5080): `engine_tests --suite editor --backend vulkan` 0, 340 assertions in 31 cases; `engine_tests --suite gpu --backend vulkan` 0, 43935 assertions in 73 cases.
- Linux: `cmake -P cmake/ValidateVendor.cmake` 0.
- Windows session: `cmd.exe /c tasklist` still lists LockApp (58728) and LogonUI (52008), so `run_ui.ps1` cannot capture the editor.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 39, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, or `cmake` is newer than this log.
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728), so the desktop is locked. No `game_editor.exe` capture or action log was run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 40, Partial)
- What was implemented: no source change in this pass. No file under `engine`, `tests`, `tools`, or `cmake` is newer than this log.
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728), so the desktop is locked. No `game_editor.exe` capture or action log was run.
- Toolchain: only g++ 9.4 and clang++ are installed; `linux-gcc` (GCC 11+) cannot run here.
- Status: stays **Partial**; no COMPLETE signal. Open: unlock the Windows session, then run `artifacts/ui/us061/run_ui.ps1` and retain the capture and action log; native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL); `linux-gcc` CI job (GCC 11+).
---

## 2026-10-25 - US-061 (Status check 41, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, or `cmake` is newer than this log.
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728). No `game_editor.exe` capture or action log was run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---
## 2026-10-25 - US-061 (Status check 42, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, or `cmake` is newer than this log.
- Linux: `ctest --preset linux-unit --output-on-failure --no-tests=error` 0, 30 of 30 passed (integration 29, unit 1), run solo.
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728), so the desktop is locked. No `game_editor.exe` capture or action log was run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 43, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than this log.
- Gates: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files).
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728), so the desktop is locked. No `game_editor.exe` capture or action log was run.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 44, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than the previous log entry.
- Gates: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files).
- Windows session: `cmd.exe /c tasklist` still lists LockApp (58728) and LogonUI (52008), so the desktop is locked. No `game_editor.exe` capture or action log was run; `artifacts/ui/us061/NOT_VERIFIED.txt` stands.
- Status: stays **Partial**; no COMPLETE signal. Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-061 (Status check 45, Partial)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, or `CMakePresets.json` is newer than the previous log entry.
- Gates: `cmake -P cmake/ValidateVendor.cmake` 0.
- Windows session: `cmd.exe /c tasklist` still lists LogonUI (52008) and LockApp (58728), so the desktop is locked. No `game_editor.exe` capture or action log was run; `artifacts/ui/us061/NOT_VERIFIED.txt` stands.
- Status: stays **Partial**; no COMPLETE signal. Stopping here: the remaining items need the user (unlock the Windows session) or a toolchain not installed here (GCC 11+). Open: unlocked-desktop GUI capture and action log (`artifacts/ui/us061/run_ui.ps1`), native Linux Vulkan editor and GPU suites (no Vulkan adapter under WSL), `linux-gcc` CI job (GCC 11+; only GCC 9.4 installed).
---

## 2026-10-25 - US-062
- What was implemented: `SceneEditor` (scene document, selection, hierarchy with filter, create/duplicate/remove/reparent, components, field and transform edits, scene file save/open, dirty state, known asset set). `EditorModel::scene()`. Hierarchy and Inspector panels, scene toolbar, drag-and-drop reparent, Euler rotation editing, unsaved-changes dialog. `Scene::component_types`.
- Files changed: engine/editor/{include/engine/editor/scene_editor.hpp, src/scene_editor.cpp, editor_model.hpp/.cpp, editor_ui.hpp/.cpp, CMakeLists.txt}, engine/scene/{include/engine/scene/scene.hpp, src/scene.cpp}, tools/game_editor/src/main.cpp (close button through the guard), tests/src/editor_scene_tests.cpp, tests/CMakeLists.txt, docs/editor.md.
- Gates: Linux `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0 (full); `ctest --preset linux-unit` 0 (30 of 30); `engine_tests --suite editor` 0 (42 cases, 446 assertions, including 13 new scene cases).
- Windows (`us062_gate.bat`, RTX 5080, MSVC): ValidateVendor 0, configure 0, win-debug build 0, win-unit ctest 0, `engine_tests --suite editor --backend dx12` 0 (43 cases, 464 assertions), `engine_tests --suite gpu --backend dx12` 0 (72 cases, 43923 assertions).
- Not verified: visual check of the running window (no unlocked desktop); linux-gcc (GCC 11+ not installed); native Linux Vulkan editor suite; asan preset not run.
- Learnings:
  - Gotcha: `-Wshadow` fails on `if (const auto* x = ...) else if (const auto* x = ...)`; give each branch its own name.
  - Gotcha: a reparent test must build a real ancestor chain. Moving one entity to the root first makes the next move legal.
  - Known limits: the hierarchy lists the document's entities, not prefab interiors; rotation is shown as Euler degrees; reparent keeps the local transform; prefab files are not loaded by the editor.
---

## 2026-10-25 - US-062 (verification pass)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, `CMakePresets.json`, or `docs` is newer than the previous US-062 entry.
- Gates re-run on Linux: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit` 0 (30 tests); `engine_tests --suite editor` 0 (42 cases, 446 assertions).
- Windows gates: as recorded in the entry above (`us062_gate.bat`); not re-run in this pass.
- Not verified: visual check of the running `game_editor` window (no unlocked desktop); `linux-gcc` (GCC 11+ not installed); native Linux Vulkan editor suite; `linux-asan` preset not run.
- Status: acceptance criteria met on the available toolchains; GUI, linux-gcc, Vulkan, and ASan items are open and reported as not run.
---

## 2026-10-25 - US-063
- What was implemented: `EditorViewport` (`viewport.hpp/.cpp`): a free `EditorCamera` (orbit, pan, dolly, fly, frame), `project_point` and `camera_ray`, entity picking by marker, and Move/Rotate/Scale gizmos in Local or World space (Scale always on the entity's own axes) with snapping. Drags write through `SceneEditor::set_transform`. The Viewport panel draws a ground grid, markers, and the gizmo with ImDrawList and reads ImGui input into `ViewportInput`. The DPI scale comes from the window (`EditorModel::set_dpi_scale`, updated on `WindowDpiChanged`). `SceneEditor::world_matrix` is new.
- Files changed: engine/editor/include/engine/editor/{viewport.hpp, editor_model.hpp, editor_ui.hpp, scene_editor.hpp}, engine/editor/src/{viewport.cpp, editor_model.cpp, editor_ui.cpp, scene_editor.cpp}, engine/editor/CMakeLists.txt, tools/game_editor/src/main.cpp, tests/src/editor_viewport_tests.cpp, tests/CMakeLists.txt, docs/editor.md.
- Gates: Linux `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (first-party warnings are errors); `ctest --preset linux-unit --output-on-failure --no-tests=error` 0 (30 of 30); `engine_tests --suite editor` 0 (56 cases, 570 assertions, 14 new viewport cases).
- Windows (`us063_gate.bat`, MSVC, RTX 5080, after fixing C4267 size_t-to-glm index conversions in viewport.cpp): ValidateVendor 0, configure 0, win-debug build 0, win-unit ctest 0 (29 of 29), `engine_tests --suite editor --backend dx12` 0 (57 cases, 588 assertions), `engine_tests --suite gpu --backend dx12` 0 (72 cases, 43923 assertions).
- Not verified: the running window (no GUI check this pass, so no screenshots or action log); the native Linux Vulkan editor suite; `linux-gcc` (GCC 11+ not installed); the ASan preset (the change touches no memory, ECS, jobs, streaming, Lua, or physics path); engine_bench (editor-only change).
- Known limits: the viewport shows markers and a grid, not rendered meshes; picking tests markers, not bounds; drags have no undo; prefab interiors cannot be picked.
- Learnings:
  - Gotcha: a `scene::` qualifier still resolves to the namespace when a parameter is named `scene`. Name lookup for `::` only considers namespaces and types.
  - Gotcha: a drag test must press on the handle at an exact projected point. Use the layout length from a first update with no press, or the handle position is a guess.
  - Gotcha: two gizmo rings meet at the same points (for example the world X and Z rings both pass through the Y axis points). Test the ring press at a point on one ring only.
---


## 2026-10-25 - US-063 (verification pass)
- What was implemented: no source change. No file under `engine`, `tests`, `tools`, `cmake`, `CMakeLists.txt`, `CMakePresets.json`, or `docs` is newer than the previous US-063 entry.
- Gates re-run on Linux: `cmake -P cmake/ValidateVendor.cmake` 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0 (30 of 30); `engine_tests --suite editor` 0 (56 cases, 570 assertions). The binary is at `build/linux-dev/tests/Debug/engine_tests`.
- Camera isolation: the viewport drives only its own `EditorCamera`. Neither `viewport.cpp` nor `scene_editor.cpp` reads or writes a scene camera; the test "A gizmo drag does not move the editor camera" covers the editor camera.
- Not verified here: Windows gates (recorded in the entry above, not re-run); the running window (no unlocked desktop); native Linux Vulkan editor suite (no Vulkan adapter under WSL); `linux-gcc` (GCC 11+ not installed); `linux-asan` (the change touches no memory, ECS, jobs, streaming, Lua, or physics path).
- Status: acceptance criteria met on the available toolchains; GUI, Vulkan, and linux-gcc items are reported as not run.
---

## 2026-10-25 - US-064
- What was implemented: undo and redo in `SceneEditor`. Every scene edit (create, duplicate, remove, reparent, add/remove component, field and transform edits) is one command that keeps the scene document before and after it. `undo()`/`redo()` load one with `Scene::load`, so GUIDs and references come back exactly, and `dirty()` follows the saved text. A new command clears the redo steps. `begin_gesture`/`end_gesture` merge a gizmo drag (`EditorViewport::update`) and an inspector value (`EditorUi::inspector_panel`) into one command. The Edit menu shows the command labels, and Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y run on the scene history when no text field has the keyboard (`EditorUi::history_shortcuts`).
- Files changed: engine/editor/include/engine/editor/{scene_editor.hpp, editor_ui.hpp}, engine/editor/src/{scene_editor.cpp, viewport.cpp, editor_ui.cpp}, tests/src/editor_scene_tests.cpp, docs/editor.md.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --build --preset linux-debug` 0 (first-party warnings are errors); `ctest --preset linux-unit` 0; `engine_tests --suite editor` 0 (64 cases, 707 assertions, 8 new history cases); `engine_tests --suite unit` 0.
- Gates (Windows, `us064_gate.bat`, MSVC, RTX 5080): ValidateVendor 0, configure 0, win-debug build 0 with no C4xxx warnings, win-unit ctest 0 (29 of 29), `engine_tests --suite editor --backend dx12` 0 (65 cases, 725 assertions), `engine_tests --suite gpu --backend dx12` 0 (72 cases, 43923 assertions).
- Not verified: the running window (no GUI check, so no screenshots or action log); the native Linux Vulkan editor suite; `linux-gcc`; `linux-asan` (the history is editor code, and it touches no ECS, jobs, streaming, Lua, or physics path).
- Known limits: each command holds a full copy of the scene document before and after it, which is simple but grows with scene size and history length. The history has no cap. A prefab interior is not in the hierarchy, so its edits go through the inspector only.
- Learnings:
  - Gotcha: a merge key must not survive its gesture. A key left on the last command would let the next drag on the same entity merge into the previous one, so `end_gesture` clears it.
  - Gotcha: a never-saved scene is dirty against the empty document. A rollback test that expects a clean state must save its baseline first.
  - Gotcha: a merged edit must keep the first command's before-state. Only the after-document is updated, so one undo returns to the press.
---

## 2026-10-25 - US-065
- What was implemented: Play, Pause, Step, and Stop on the Viewport toolbar. `PlaySession` (`play_session.hpp`) is a runtime world: a copy of the scene document in its own `scene::Scene`, a physics world, an offline audio engine, a script engine, and a runner. `EditorModel` owns at most one (`play`, `toggle_pause`, `step`, `stop`, `tick_play`, `play_state`, `scene_editable`). Play never edits the editor's scene, so Stop restores the edit state. While a session runs, the scene is read-only (Hierarchy and Inspector disabled, scene undo/redo off), and the viewport keeps its camera but gets no press. Open, create, close, New, Open, and Exit stop the session first. `Scene::handle` and `Scene::registry` are new (scene.hpp, scene_graph.hpp).
- Files changed: engine/editor/include/engine/editor/{play_session.hpp, editor_model.hpp, editor_ui.hpp}, engine/editor/src/{play_session.cpp, editor_model.cpp, editor_ui.cpp}, engine/editor/CMakeLists.txt (links engine_physics, engine_script, engine_audio), engine/scene/include/engine/scene/{scene.hpp, scene_graph.hpp}, engine/scene/src/scene.cpp, tests/src/editor_play_tests.cpp, tests/CMakeLists.txt, docs/editor.md.
- Gates (Linux): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (first-party warnings are errors); `ctest --preset linux-unit --output-on-failure --no-tests=error` 0 (30 of 30); `engine_tests --suite unit` 0 (874 cases); `engine_tests --suite editor` 0 (70 cases, 1048 assertions, 7 new play cases including 20 Play/Stop cycles).
- Gotchas:
  - The first run of the new tests hung at exit. The cause was member order: the audio engine was destroyed before the script engine that stops voices in it. Fixed by declaration order; the header comment says why.
  - A never-saved scene is dirty, so a test that checks the dirty state after Stop must save a baseline first.
  - `core::Result` has no implicit conversion from a value; the create path returns `core::success(std::move(session))`.
- Gates (Windows, `us065_gate.bat`, MSVC, RTX 5080): ValidateVendor 0; configure 0; win-debug build 0; win-unit ctest 0 (29 of 29 at the time of the run, 1 Catch2 suite passed); `engine_tests --suite editor --backend dx12` 0; `engine_tests --suite gpu --backend dx12` 0 (72 cases, 43923 assertions).
- Gates (linux-asan): `cmake --preset linux-asan` and build 0; `ctest --preset linux-sanitize` 0 (30 of 30).
- FAILING: `engine_tests --suite editor --backend vulkan` returns 1 (70 of 71 cases pass). `engine_tests --suite gpu --backend vulkan` returns 1 (73 assertions fail). Every failure is `VUID-VkDeviceCreateInfo-pNext-pNext`: `vkCreateDevice` gets a `VkPhysicalDeviceMeshShaderFeaturesEXT` (sType 1000328000) in its pNext chain, and the validation layer rejects it. The chain comes from `vulkan_device.cpp` (around lines 597-612). US-065 does not touch the RHI. This is an RHI device-creation issue that is not yet traced to its cause. The story stays open until it is fixed or shown to be environmental.
- Not verified or not done:
  - Animation and GPU particles are not advanced by Play. The editor's component catalog has no component that binds them to an entity, so the session has nothing to advance for them yet. Entity-bound bodies, scripts, and animation are likewise not created from the scene.
  - The play world's audio is offline, so Play makes no sound in the editor.
  - GPU live-resource counts: Play creates no RHI objects, so the counts do not grow by construction. No GPU measurement was taken for this story.
  - No GUI check of the Play toolbar in a running window (desktop state from the earlier US-061 notes applies); no screenshots or action log.
  - engine_bench: not run; Play is editor-only and touches no render or physics hot path.
  - linux-gcc: GCC 9.4 is the only GCC installed and cannot compile Jolt, so that job is not run here.
---

## 2026-10-25 - US-065 (Vulkan verification pass)
- What was implemented: no source change. The earlier FAILING entry was environmental. The batch ran with the installed SDK layer 1.3.224, which rejects the mesh-shader feature chain (see reference-vulkan-validation-layer-windows). Re-run with `VK_LAYER_PATH=D:\dev\vvl_build\layer` (pinned 1.4.363 layer, already built): no source change was needed.
- Gates (Windows, `us065_vk.bat`, MSVC, RTX 5080, validation layers on): ValidateVendor 0; `cmake --build --preset win-debug` 0; `engine_tests --suite editor --backend vulkan` 0 (71 of 71 cases, 1066 assertions); `engine_tests --suite gpu --backend vulkan` 0 (73 of 73 cases, 43935 assertions).
- Still not verified: native Linux Vulkan (no usable Vulkan device in WSL); GUI check of the Play toolbar with screenshots; 20-cycle GPU live-resource count on the GPU (Play creates no RHI objects); engine_bench (not a performance-affecting change).
---

## 2026-10-25 - US-066
- What was implemented: the Assets panel (catalog with search, texture thumbnails drawn as coloured cells, import settings editor, import with progress and cancel, diagnostics with codes, rename and delete with a confirmation listing affected references, hot reload by directory watch). Inspector Guid fields accept a drag of a registered asset; dropping one on the viewport places a new entity with the first catalog component whose Guid field is named after the importer. `AssetBrowser` (no ImGui dependency), `ProjectSession::rescan`, `EditorModel` asset commands and `place_asset`. Converter: `Options::cancel`/`completed`, `Cancelled` code, `make_texture_preview`. Assets module: `update_asset_metadata`, `move_asset`, `remove_asset`.
- Files changed: engine/assets/{include/engine/assets/asset_project.hpp, src/asset_project.cpp}; tools/asset_converter/{include/asset_converter/converter.hpp, src/converter.cpp, src/texture_importer.cpp}; engine/editor/{CMakeLists.txt, include/engine/editor/{asset_browser.hpp, editor_model.hpp, editor_ui.hpp, project.hpp}, src/{asset_browser.cpp, editor_model.cpp, editor_ui.cpp, project.cpp}}; tests/CMakeLists.txt, tests/src/editor_asset_tests.cpp; docs/editor.md.
- Gates (Linux): ValidateVendor 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit` 30 of 30; `engine_tests --suite editor` 81 cases, 1261 assertions (11 new asset cases); `ctest --preset linux-sanitize` (ASan/UBSan build) 30 of 30.
- Gates (Windows, `us066_gate.bat`, MSVC, RTX 5080, after the fix below): ValidateVendor 0; `cmake --preset win-dev` 0; `win-debug` build 0; `win-unit` ctest 29 of 29; `engine_tests --suite editor --backend dx12` 82 cases, 1279 assertions; `--backend vulkan` (pinned 1.4.363 layer) 82 cases; `--suite gpu --backend dx12` 72 cases, 43923 assertions; `--suite gpu --backend vulkan` 73 cases, 43935 assertions.
- Not verified: the running window (no GUI in this session, so no screenshots or action log for drag/drop, the popup, or hot reload); native Linux Vulkan editor and GPU suites (no usable Vulkan device under WSL); linux-gcc (GCC 9.4 only); engine_bench (no render or physics hot path changed).
- Known limits: the viewport drop depends on the game registering a component with a Guid field named after the importer; the drop is checked only for "a registered asset", not for kind; json and text sources are the only ones searched for references; delete leaves cooked output in place; models and materials have no preview.
- Learnings:
  - Gotcha: moving a `selected` read below a loop that still uses it broke the Windows and ASan builds, and the batch ran stale test binaries. Check `build=` before trusting test lines.
  - Gotcha: `converter::Options` has no default constructor (`core::Path` member), so it is built with all fields in order.
  - Gotcha: `ProjectSession::open` must keep its InvalidPath branch; the rescan path does not own it.
---

## 2026-10-25 - US-067
- What was implemented: a versioned material graph (`engine.material_graph` v1) with constants, parameters, UVs, texture sampling (sRGB or linear, with a per-texture fallback), arithmetic, normal maps, and a PBR output. `analyze()` reports node-specific type, pin, cycle, and output errors. `generate_hlsl` emits one source for six variants (blend mode x two-sided), compiled for DXIL and SPIR-V through the pinned DXC and the pipeline cache. `SurfaceEvaluator` is the CPU reference, with the same conversions, fallbacks, and clamps as the generated code. Instance parameters go through `pack_parameters` into the block, so a value change is a cache hit; a topology or constant change gives new keys.
- Files changed: new `engine/material_graph/` (CMakeLists.txt, include/engine/material_graph/{graph,codegen,evaluate,compile}.hpp, src/{graph,codegen,evaluate,compile}.cpp); `CMakeLists.txt` (add_subdirectory); `tests/CMakeLists.txt` (sources, link, DXC block); `tests/src/material_graph_fixture.hpp`, `tests/src/material_graph_tests.cpp` (unit), `tests/src/material_graph_compile_tests.cpp` (integration, DXC); `docs/material-graph.md`.
- Gates (Linux): ValidateVendor 0 (26 deps, 8542 files); `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit` 0 (all 30 entries); `engine_tests --suite unit` 906 cases, all passed (256642 assertions); `engine_tests --suite integration` 122 cases, 117 passed, 5 skipped (the X11 window-manager tests, skipped before this change).
- Gates (Windows, `us067_gate.bat`, MSVC): ValidateVendor 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (the new sources compiled under /W4 /WX with no warnings); `ctest --preset win-unit` 0 (29 of 29). GPU suites not run: no GPU path changed.
- Not verified: `linux-asan` and `linux-gcc` (the change touches no memory, ECS, jobs, streaming, Lua, or physics path; GCC 9.4 cannot build Jolt, so linux-gcc is blocked as before); engine_bench (no render or physics hot path changed); GPU suites (no GPU path changed; the generated PSGraph is not bound to any pipeline yet); native Linux Vulkan.
- Known limits: the scene renderer does not draw graph materials yet; the visual editor (US-068) is the first consumer. No GPU parity test between the generated shader and the CPU reference yet. Node positions are not part of the format.
- Learnings:
  - Gotcha: textures at t0 and the cbuffer at b0 in set 1 gave both binding 0, and `validate_bindings` reported the type mismatch on the texture. Binding numbers must be unique within the set.
  - Gotcha: an unbound normal-map texture sampled white, which decodes to a tilted normal (0.577, 0.577, 0.577). The renderer's default for a normal slot is flat, so each texture declares its fallback.
  - Gotcha: unused cbuffer members and textures may be dropped from the DXC reflection, so the reflection check runs only when the block is present, and `validate_bindings` allows unused declared bindings.
---

## 2026-10-25 - US-066 (verification pass)
- What was implemented: no source change. US-066 was already in place; this pass re-checked it against the acceptance criteria.
- Gates re-run on Linux: `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 0; `engine_tests --suite editor` 0 (81 cases, 1261 assertions).
- Windows gates: as recorded in the US-066 entry above (`us066_gate.bat`); not re-run in this pass.
- Not verified: the running window (no GUI, so no screenshots or action log); native Linux Vulkan suites; `linux-gcc` (GCC 9.4 only); engine_bench (no render or physics hot path changed).
---

## 2026-10-25 - US-068
- What was implemented: the editable material graph. `GraphDocument` (headless) adds, removes, links, unlinks, and edits nodes, with undo/redo, selection, a separate layout document, last-valid preservation, save and open round trips, and a CPU preview of the last valid graph. The editor's Material Graph window (View menu) draws it as a node canvas with pins, links, selection, moves, link dragging, unlink, a properties column, the error list (clicking one selects its node), and a sphere preview shaded from the evaluator.
- Files changed: engine/material_graph/{CMakeLists.txt, include/engine/material_graph/edit.hpp, src/edit.cpp, src/graph.cpp (analyzer indexes nodes before checking links)}; engine/editor/{CMakeLists.txt, include/engine/editor/material_graph_panel.hpp, include/engine/editor/editor_ui.hpp, src/material_graph_panel.cpp, src/editor_ui.cpp}; tests/CMakeLists.txt, tests/src/material_graph_edit_tests.cpp; docs/material-graph.md.
- Gates (Linux): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit` 30 of 30; `engine_tests --suite unit` 919 cases, all passed (256749 assertions); `engine_tests --suite editor` 81 cases, 1261 assertions.
- Gates (Windows, `us068_gate.bat`, MSVC /W4 /WX, D:\dev tree): ValidateVendor 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (new sources compiled with no warnings); `ctest --preset win-unit` 0 (29 of 29).
- Not verified: the running window (no GUI in this session, so no screenshots, action log, or interactive check of the canvas); `engine_tests --suite editor` on Windows; GPU suites (no GPU path changed); engine_bench (no hot path changed); linux-asan (the change touches no memory, ECS, jobs, streaming, Lua, or physics path); linux-gcc (GCC 9.4 cannot build Jolt); native Linux Vulkan.
- Known limits: the preview is a CPU sphere, not a GPU mesh, and it does not shade normal maps or roughness. The material window is not saved in the workspace (a View toggle only). The editor does not cook a graph; `compile_material` is not wired to the editor, so no cooked output is produced from the window yet. The canvas has no zoom or box selection.
- Learnings:
  - Gotcha: `analyze` checked each link while it was still building the id index, so a link to a later node was reported as unknown. The editor appends nodes freely, so the index now comes first.
  - Gotcha: `parse_graph` runs the whole graph check, so a graph with errors cannot be opened. Errors only come from edits, and the document keeps the last valid version.
  - Gotcha: `std::erase_if` is C++20 in the library, and the local GCC 9 libstdc++ lacks it. Use remove_if and erase.
---

## 2026-10-25 - US-069
- What was implemented: the animation state machine runtime. A versioned JSON state graph (`engine.animation_graph`, version 1) with float/bool/trigger parameters, conditions (float compare, bool, trigger, finished), explicit default state, explicit per-source priorities, and timed blends (duration > 0). `StateMachine` runs it at a fixed step: loop and finish events, one transition at a time, blended pose, and root-motion deltas that stay continuous through a blend. `AnimatorSet` holds named machines. The Lua `animation` module (in `engine_script`) sets and reads parameters on those machines and reports state.
- Files changed: engine/animation/{CMakeLists.txt, include/engine/animation/state_graph.hpp, include/engine/animation/state_machine.hpp, src/state_graph.cpp, src/state_machine.cpp}; engine/script/{CMakeLists.txt, include/engine/script/script_engine.hpp (ScriptServices::animators), src/script_bindings.cpp (animation module)}; tests/CMakeLists.txt; tests/src/animation_state_machine_tests.cpp, tests/src/script_animation_tests.cpp; docs/animation.md.
- Gates (Linux, clang): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0; `ctest --preset linux-unit` 0; `engine_tests --suite unit` 938 cases, all passed (256945 assertions).
- Gates (Linux, ASan/UBSan): `cmake --preset linux-asan` 0; build 0; `ctest --preset linux-sanitize` 0 (30 of 30). The Lua binding is in the sanitized set.
- Gates (Windows, `us069_gate.bat`, MSVC /W4 /WX): ValidateVendor 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (no warnings); `ctest --preset win-unit` 0 (29 of 29).
- Not verified: `linux-gcc` (GCC 9.4 cannot build Jolt, as before); `linux-release` and engine_bench (no render or physics hot path changed; the state machine is not in a benchmark workload yet); GPU suites (no GPU path changed); native Linux Vulkan.
- Known limits: no transition is taken while a blend runs, and blends cannot be interrupted. Loop and finish events come from the active clip only. No blend trees or sub-state machines. Clips are built in code; no clip importer is involved yet.
- Learnings:
  - Gotcha: Catch2 `INFO` takes a streamable expression, not a ternary with `std::string` and a `const char*` mix. Compute the message into a local string first.
  - Gotcha: a loop-event boundary and a finish edge must be computed after the time advance and before the transition check, so a clip that finishes in an update takes its finished-condition transition in that same update.
---

## 2026-10-25 - US-070
- What was implemented: the animation graph document (`AnimationGraphDocument`, headless): states, transitions, parameters, and conditions are created and edited. Every edit builds a candidate graph and runs `StateMachine::create` on it, so a refused edit changes nothing. Clip and skeleton compatibility use `validate_clip`, and `compatible_clips()` feeds the clip picker. Undo and redo, save, and open are in the document. The Animation Graph window (View > Animation Graph) draws it with a preview (state, blend, timeline, parameter controls) and a Play monitor. `PlaySession` now owns an `AnimatorSet` that the Lua `animation` module reaches, and it steps the set on each fixed step after the scripts.
- Files changed: engine/animation/{CMakeLists.txt, include/engine/animation/graph_edit.hpp, src/graph_edit.cpp}; engine/editor/{CMakeLists.txt, include/engine/editor/{animation_graph_panel.hpp, editor_ui.hpp, play_session.hpp}, src/{animation_graph_panel.cpp, editor_ui.cpp, play_session.cpp}}; tests/CMakeLists.txt, tests/src/animation_graph_edit_tests.cpp (new, 15 cases), tests/src/editor_play_tests.cpp (1 new Play case); docs/animation.md, docs/editor.md.
- Gates (Linux): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (no warnings); `ctest --preset linux-unit` 29 of 30 on the final run: the one failure was `engine_tests_integration`, an X11 cursor-capture check that failed because another client held the pointer (environmental); its rerun passed 0; `engine_tests --suite unit` 954 cases, all passed; `engine_tests --suite editor` 82 cases, all passed; `linux-asan` configure and build 0; `ctest --preset linux-sanitize` 0 (30 of 30).
- Gates (Windows, `us070_gate.bat`, MSVC /W4 /WX): ValidateVendor 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (after the fix below); `ctest --preset win-unit` 0. `engine_tests --suite editor` 0 (82 cases, 1277 assertions); `engine_tests --suite editor --backend dx12` 0 (83 cases, 1295 assertions). The first batch failed the build (`build=2`) and its `unit=0` ran the old binary, so it is not counted.
- Not verified: the running window (no GUI in this session, so no screenshots, action log, or interactive check of the window or the Play monitor); `--backend vulkan` on Windows and native Linux (no usable Vulkan device here); linux-gcc (GCC 9.4 cannot build Jolt, as before); engine_bench (no render or physics hot path changed; the Play change adds one empty `AnimatorSet::update` per fixed step).
- Known limits: the window opens graphs only against the built-in demo rig (two joints, idle/walk/jump), because there is no clip importer, so its skeleton picker has one entry. Transitions are edited from lists, not on a canvas. Numeric fields commit on Enter, so each typed value is one undo entry. A "New" or "Open" discards unsaved window edits without a prompt. Entity-bound animation is still not created from a scene.
- Learnings:
  - Gotcha: `physics::kFixedStep` is a double, and `AnimatorSet::update` takes a float. MSVC /W4 /WX made the implicit narrowing (C4305) an error that clang did not report, so the Windows build failed and the batch's `unit=0` ran a stale binary. Cast at the call.
  - Gotcha: a machine's skeleton must match its clips' track count, and `validate_skeleton` also checks each inverse bind against the bind pose. A fixture whose child joint has an identity inverse bind fails at create, so build the inverse from the bind translation.
  - Gotcha: the Catch2 fixture helper `REQUIRE(...)` inside a helper that returns a value is fine, but `REQUIRE` with a chained `||` gave no useful message; keep one check per line.
---

## 2026-10-25 - US-071
- What was implemented: headless terrain sculpting and painting. `TerrainDocument` (engine/world) applies raise, lower, smooth, flatten, and four-layer paint stamps with radius, strength, and falloff (`brush_weight`). A stroke is one undo command: each touched sample keeps its before and after value. Touched tiles are reported by `take_dirty_tiles()` with per-tile revisions. `refresh_terrain_collider` (engine/physics) replaces a tile body's heightfield in place. `.terrain` files save and load atomically (`save_terrain`/`load_terrain`, `serialize_terrain`/`parse_terrain`).
- Files changed: engine/world/{CMakeLists.txt, include/engine/world/terrain_edit.hpp, src/terrain_edit.cpp}; engine/physics/{include/engine/physics/collision_cells.hpp, src/collision_cells.cpp}; tests/CMakeLists.txt, tests/src/world_terrain_edit_tests.cpp (new, 14 cases), tests/src/converter_world_tests.cpp (1 integration case: edit, save, reopen, cook gives identical blobs); docs/terrain-editing.md.
- Gates (Linux, clang): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (no warnings); `ctest --preset linux-unit` 0; `engine_tests --suite unit` 967 of 967; `--suite integration` 1586 assertions, all passed. `linux-asan` configure, build 0; `ctest --preset linux-sanitize` 0 (30 of 30).
- Gates (Windows, `us071_gate.bat`, MSVC /W4 /WX): ValidateVendor 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (0 warnings); `ctest --preset win-unit` 0 (29 of 29).
- Not verified: the editor UI (no terrain panel exists; no GUI session); GPU suites and LOD/RT consumers (the renderer does not read dirty tiles yet); engine_bench; linux-gcc (GCC 9.4 cannot build Jolt, as before); native Linux Vulkan.
- Known limits: no editor tool, so the brushes are driven from code only. Heightfield colliders are quantised by Jolt (about 2 mm error), so the collider test uses a 1 cm tolerance. Smooth reads neighbours from the pre-stamp state; the stroke is not order-dependent within one stamp, but stamps in a stroke apply in sequence.
- Learnings:
  - Gotcha: a `PhysicsWorld& world` parameter next to `world::Terrain` compiles, but it reads badly; name it `physics`.
  - Gotcha: Catch2 `Catch::Approx` needs `<catch2/catch_approx.hpp>` included explicitly in this tree.
  - Gotcha: Jolt heightfields quantise samples, so a raycast height differs from the sample by about 2 mm.
---

## 2026-10-25 - US-071 (verification pass, status Partial)
- What was checked: no source change. `cmake --build --preset linux-debug` succeeds; `engine_tests --suite unit` 967 of 967 passed, including the terrain edit cases.
- Status call: US-071 stays **Partial**, not COMPLETE. Two acceptance points are not met:
  - Editor UI: nothing in `engine/editor` references `TerrainDocument`, so brushes are reachable from code only, not from the editor as the story asks.
  - LOD/RT invalidation: `take_dirty_tiles()` and `tile_revision()` exist, but no consumer rebuilds LOD meshes or RT geometry from them yet.
- Open: the editor brush panel and its tests; the LOD/RT consumer; the Windows and Linux gates recorded above were not rerun here.
---

## 2026-10-25 - US-071 (editor panel and LOD/RT invalidation, status Complete)
- What was implemented: the terrain editor session `TerrainEditor` (headless, engine/editor): one `TerrainDocument`, the brush settings, a lazily built LOD-0 mesh per tile keyed by the document's tile revision, and one physics collider per tile (`ensure_colliders`). `refresh(physics)` drops the meshes of the dirty tiles and refreshes every body whose revision is behind, so a failed body is retried. The Terrain window (`TerrainPanel`, View > Terrain) creates, opens, and saves a terrain, edits the brush, undoes and redoes, and paints strokes on a top-down map (the pointer is the brush; one stroke is one undo command; closing the window mid-stroke ends it).
- Fix found while reviewing: a sample's normal reads both neighbours, so an edit one sample inside a tile changed the adjacent tile's edge normal without dirtying that tile. `mark_samples` now expands the footprint by one sample each way. Regression test: `An edit one sample inside a tile dirties the neighbour whose edge normal reads it`.
- Files changed: engine/world/src/terrain_edit.cpp; engine/editor/{CMakeLists.txt, include/engine/editor/{terrain_editor.hpp, terrain_panel.hpp, editor_ui.hpp}, src/{terrain_editor.cpp, terrain_panel.cpp, editor_ui.cpp}}; tests/CMakeLists.txt, tests/src/editor_terrain_tests.cpp (new, 8 cases), tests/src/world_terrain_edit_tests.cpp (1 case); docs/terrain-editing.md.
- Gates (Linux): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (no warnings); `ctest --preset linux-unit` 0; `engine_tests --suite unit` 976 cases passed; `engine_tests --suite editor` 90 cases passed; `linux-asan` configure and build 0; `ctest --preset linux-sanitize` 0 (no sanitizer reports).
- Gates (Windows, `us071_gate.bat`, MSVC /W4 /WX, RTX 5080): validate 0; configure 0; win-debug build 0 (0 warnings, terrain sources compiled); win-unit ctest 0; `engine_tests --suite editor --backend dx12` 0 (91 cases, 1393 assertions); `--suite editor --backend vulkan` 0 (91 cases, pinned 1.4.363 layer via VK_LAYER_PATH); `--suite gpu --backend dx12` 0 (72 cases, 43923 assertions); `--suite gpu --backend vulkan` 0 (73 cases, 43935 assertions).
- Not verified: the running window (no GUI session, so no screenshots or action log for the Terrain window); native Linux Vulkan (no usable device under WSL); linux-gcc (GCC 9.4 cannot build Jolt); engine_bench (no render or physics hot path changed).
- Known limits: the window has no physics world, so its edits refresh meshes only, not colliders. Terrain is not drawn in the scene viewport, and no GPU upload of invalidated meshes exists yet, so the RT/LOD consumers are the mesh rebuild contract, not a live GPU path. All editor tiles render at LOD 0, so stitching is not exercised there. "New" discards an unsaved terrain without a prompt.
- Learnings:
  - Gotcha: a per-sample dirty footprint must include the neighbours the normal reads, or the adjacent tile is stale without being dirty.
  - Gotcha: the `find build -name engine_tests` picked a stale ASan binary. Use the preset's binary path (`build/linux-dev/tests/Debug/engine_tests`) for the unit and editor counts.
---

## 2026-10-26 - US-072 - World Partition Editor
- What was implemented: a headless `WorldEditor` (engine/editor) that owns the authored world in memory, adds and moves objects (GUIDs, names, and references kept; both cells of a move dirty), saves dirty cells plus `world.json` (the grid, which no cell file holds), and drives residency through a `StreamingManager` over a derived `.stream` cache rebuilt from the world. Residency controls: Load (pin, clear suppression), Pin, Unload (suppress, clear pin), Go to cell (editor camera at the cell centre). `StreamingManager` gained `set_pinned`/`set_suppressed` (streaming.cpp), so the editor can keep a cell resident outside the ring or drop it inside the window. The `WorldPanel` (View > World) draws a cell map coloured by phase with P/S/*/C markers, the cell controls, an object list with a position move, an add-object form, and the streaming overlay (budgets, window, residency and stage counters, camera).
- Files changed: engine/world/src/streaming.cpp, engine/world/include/engine/world/streaming.hpp; engine/editor/CMakeLists.txt, include/engine/editor/{world_editor.hpp, world_panel.hpp, editor_ui.hpp}, src/{world_editor.cpp, world_panel.cpp, editor_ui.cpp}; tests/CMakeLists.txt, tests/src/editor_world_tests.cpp (new, 8 cases), tests/src/world_streaming_tests.cpp (2 cases); docs/world-editing.md.
- Gates (Linux, clang 10): ValidateVendor 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (no warnings); `ctest --preset linux-unit` 30 of 30 passed; `engine_tests --suite unit` 986 cases passed (976 earlier plus 10 new); `engine_tests --suite editor` 98 cases passed (90 earlier plus 8 new); `linux-asan` configure and build 0; `ctest --preset linux-sanitize` 30 of 30 passed, no sanitizer reports.
- Gates (Windows, `us072_gate.bat`, MSVC /W4 /WX, RTX 5080): validate 0; configure 0; win-debug build 0; `ctest --preset win-unit` 0; `engine_tests --suite editor --backend dx12` 0 (99 cases); `--suite editor --backend vulkan` 0 (pinned 1.4.363 layer); `--suite gpu --backend dx12` 0; `--suite gpu --backend vulkan` 0 (73 cases). A later rebuild (`us072b_gate.bat`, after the wide-world selector edit) gave win-debug build 0 and editor dx12 0.
- Not verified: the running window (no GUI session, so no screenshots or action log for the World window); native Linux Vulkan; linux-gcc (GCC 9.4 cannot build Jolt); engine_bench (no benchmark harness exists yet, US-074); native Linux Vulkan GPU runs were not done (no usable device under WSL).
- Known limits: no undo for world edits (terrain has its own). Opening a world loads every cell into the authoring copy; the stream budget governs the runtime copy only. The viewport does not draw cells, and the world camera is a position, not the viewport's orbit camera. The editor's streaming manager has no subscriber, so a residency change runs no gameplay script; Play is the only script host.
- Learnings:
  - Gotcha: `core::failure("literal")` does not convert to `Result<..., std::string>`; the literal decays to `const char*`.
  - Gotcha: a test that asserts a cell is resident must check the cell that is pinned or in the window; a moved object's new cell outside the ring is correctly unloaded.
  - Gotcha: a `core::Result` is not an optional. Do not write `*result`; check `ok()`, then read `value()`.
---

## 2026-10-26 - US-073 - Game Distribution from the Editor
- What was implemented: a headless `build_game` (engine/editor/src/distribution.cpp) that validates the project, cooks it into `<output>/<name>.build/cooked`, packs the start scene's closure, assembles `<output>/<name>.staging` (game_runtime, data/game.pack, shaders/, NOTICES.txt, the launcher, game.json with every file's size and SHA-256), copies that package into an empty folder and starts the runtime there (the scene must report itself), then publishes it. `GameBuilder` runs the build on a worker with stage, cook progress, and cancel. The editor has File > Build game... (a Build Game window with output folder, package name, start scene GUID, backend, progress, Cancel). NOTICES.txt is generated at configure time from third_party/manifest.json. Docs: docs/distribution.md.
- Files changed: engine/editor/include/engine/editor/{distribution.hpp, editor_ui.hpp}; engine/editor/src/{distribution.cpp, editor_ui.cpp}; engine/editor/CMakeLists.txt (sources, engine_pack link, runtime and shader defines, notices generation); tools/game_editor/CMakeLists.txt (depends on game_runtime); tests/CMakeLists.txt (new test file, runtime dependency, fixture define); tests/src/editor_distribution_tests.cpp (new, 7 cases); docs/distribution.md (new); docs/runtime-packs.md (Build Game pointer).
- Gates (Linux, clang 10): ValidateVendor 0; `cmake --preset linux-dev` 0; `cmake --build --preset linux-debug` 0 (no warnings); `ctest --preset linux-unit` 0; `engine_tests --suite editor` 105 cases passed (98 earlier plus 7 new). A manual run of the Linux launcher script from `/` started the packed scene (exit 0).
- Not verified here: linux-gcc and linux-asan (not run; the change adds file, process, and async code and no memory-model change, but the sanitizer preset was not run). Native Linux Vulkan GPU runs (no usable device under WSL). The Build Game window was not driven in a running GUI, so no screenshots or action log exist. engine_bench (no harness yet, US-074).
- Known limits: the package is native only; payloads are uncompressed; the runtime reads no settings file, so the launcher carries the arguments; the verify step launches with `std::system`. Game code cannot yet add component types, so a scene with components is refused at launch and a package of such a scene fails verification.
- Learnings:
  - Gotcha: a `tests/CMakeLists.txt` define that reads a variable set further down the file gets an empty value. Use the source-relative path.
  - Gotcha: `engine::core::Path` has no default constructor, so option structs that hold it cannot be default-constructed. The build options hold UTF-8 strings and convert once at the start.
  - Gotcha: `std::copy_file` ADL-collides with a local helper of the same name; the helper's fs call resolves to itself.
---
- Gates (Windows, `us073_gate.bat`, MSVC /W4 /WX, RTX 5080): validate 0; configure 0; win-debug build 0 (0 warnings); `ctest --preset win-unit` 0; `engine_tests --suite editor --backend dx12` 0 (106 cases, 1520 assertions, includes the Windows build-game launch check); `--suite editor --backend vulkan` 0 (pinned 1.4.363 layer via VK_LAYER_PATH).

## 2026-10-26 - US-073 (re-verification)
- Re-ran Linux gates on the current tree: ValidateVendor 0; `linux-dev` configure 0; `linux-debug` build 0 with 0 warnings; `ctest --preset linux-unit` 0; `engine_tests --suite editor` 105 cases passed (1502 assertions).
- `linux-asan` configure, build 0; `ctest --preset linux-sanitize` 30 of 30 passed, no ASan/UBSan reports. This closes the sanitizer gap noted above.
- Still not run: linux-gcc (GCC 9.4 cannot build Jolt), native Linux Vulkan GPU, engine_bench (no harness, US-074), and a driven GUI session for the Build Game window.
---

## 2026-10-26 - US-074 - Benchmark Harness
- What was implemented: `engine_bench` (tools/benchmark) and the `engine_benchmark` library. The versioned manifest `benchmarks/manifest.json` declares the scene workloads (seeds, counts, route names), the image and physics profiles, and the smoke and acceptance suites with the contract timing. `parse_manifest` checks it; the report echoes each scene and profile definition and the manifest SHA-256. Measurement: nearest-rank p50/p95/p99/max for CPU and GPU, `run_frames` (start-to-start intervals after warmup), `evaluate_contract` (p99 <= 1000/60 ms, >= 99% of frames within it, <= 0.1% over 1000/30 ms, no frame over 100 ms, residency <= 12 GiB GPU and 16 GiB host). Reports: `report.json` plus `raw/<run>.csv` per repeat. Exit codes: 0 passed, 1 threshold failed, 2 usage/manifest error, 3 unsupported/missing/incomplete. The only implemented workload is a seeded `calibration` CPU self-check (smoke suite).
- Files changed: benchmarks/manifest.json (new); tools/benchmark/CMakeLists.txt, include/engine/benchmark/{manifest,measure,report,cli}.hpp, src/{manifest,measure,report,cli,main}.cpp (new); CMakeLists.txt (add_subdirectory); tests/CMakeLists.txt (bench_tests.cpp, engine_benchmark link, ENGINE_TEST_BENCH_MANIFEST); tests/src/bench_tests.cpp (new, 16 cases); docs/benchmarks.md (new).
- Gates (Linux, clang 10): ValidateVendor 0; `linux-dev` configure 0; `linux-debug` full build 0 with no warnings; `ctest --preset linux-unit` 0 (engine_tests --suite unit 1002 cases passed). Smoke run: exit 0, raw CSV and report written. Acceptance on this WSL host: vulkan and dx12 both `unsupported` (exit 3). A copied manifest with the calibration scene set to `none` gives `missing` (exit 3). A bad manifest gives exit 2.
- Not done: no acceptance workload exists (render_stress, physics_stress, streaming_stress, all_features report missing until US-075 to US-078 and the final scene). GPU timestamps and residency are not collected, so no acceptance run can pass yet. No Release measurement was taken. The Linux ASan preset was not run (the harness touches no memory-model, ECS, jobs, streaming, Lua, or physics code beyond allocations). Driver version is not recorded; Windows OS build is recorded as not recorded.
- Learnings:
  - Gotcha: `core::failure("literal")` decays to `const char*`; the manifest parser needed `std::string(...)` on four lines.
  - Gotcha: a scene's manifest fields are validated at parse time (the builtin check needs `items` and `rounds_per_frame`) because the run reads them with `.at()`.
  - Gotcha: a timing failure must stay Failed when residency is not measured; returning Incomplete there hides a failed repeat.
---

## 2026-10-26 - US-074 (re-verification)
- Linux (clang 10): ValidateVendor 0; `linux-dev` configure 0; `linux-debug` build 0 with 0 warnings; `ctest --preset linux-unit` 0 (30 of 30 passed).
- Windows (`us074_gate.bat` via cmd.exe, MSVC): ValidateVendor 0; `win-dev` configure 0; `win-debug` build 0 with 0 "warning C" lines; `ctest --preset win-unit` 0 (29 of 29 passed).
- Still open, as the earlier entry says: no acceptance workload exists, so `engine_bench --suite acceptance` cannot pass (exit 3 is the correct result until US-075 to US-078 add the workloads). linux-gcc and linux-asan were not re-run here.
---

## 2026-10-26 - US-075 - Reproducible Test Assets
- What was implemented: a seeded workload generator for the four benchmark scenes (`render_stress`, `physics_stress`, `streaming_stress`, `all_features`). The table's counts drive it: unique meshes (8 to 32 quads a side), instance descriptors over the 8192 m world, animated characters with a binary-tree skeleton and a 60-keyframe clip, local lights and suns, particle emitters (1000 live particles each), physics bodies, static colliders, constraints, and physics characters, audio emitters, and the shared 9 x 9 tile terrain window (257 x 257 samples at 1 m). Every component has a SHA-256 digest. `engine_workload --write` writes the expected file; `--check` regenerates and compares it. The expected file `benchmarks/workload_expected.json` is checked in. A fixture provenance table lists the 39 files under `tests/fixtures` with SHA-256, source, and license, and a unit test verifies it.
- Files changed: tools/benchmark/include/engine/benchmark/workload.hpp (new); tools/benchmark/src/{workload.cpp, workload_main.cpp} (new); tools/benchmark/CMakeLists.txt (workload source, engine_workload target); benchmarks/workload_expected.json (new); tests/fixtures/PROVENANCE.md (new); tests/src/{bench_workload_tests.cpp, fixture_provenance_tests.cpp} (new, 14 cases); tests/CMakeLists.txt (sources, ENGINE_TEST_WORKLOAD_EXPECTED); docs/benchmarks.md (workloads, fixture provenance, license note).
- Gates (Linux, clang 10): ValidateVendor 0; `linux-dev` configure 0; `linux-debug` build 0 with 0 warnings; `ctest --preset linux-unit` 30 of 30 passed; `engine_tests --suite unit` 1016 cases (1002 earlier plus 14 new); `engine_workload --check` 0 (4 scenes match).
- Gates (Windows, `us075_gate.bat` via cmd.exe, MSVC /W4 /WX): ValidateVendor 0; `win-dev` configure 0; `win-debug` build 0 with 0 "warning C" lines (new sources compiled); `ctest --preset win-unit` 0 (29 of 29 passed); `engine_workload --check` 0. The expected file written on Linux matches MSVC's output exactly, which confirms the integer-only generation is portable.
- Not verified: linux-asan (no memory, ECS, jobs, streaming, Lua, or physics code changed; the generator allocates one 21 MB terrain buffer, built once per process); linux-gcc (GCC 9.4 cannot build Jolt, as before; engine_benchmark does not depend on physics, but the job was not run); engine_bench (the acceptance scenes still report `missing`, because the scene builders and route are US-076 to US-078; no performance result exists); the GPU suites (no GPU path changed); native Linux Vulkan.
- Audio reproduction: the four audio commands in `tests/fixtures/audio/README.md` were run with ffmpeg in a scratch directory. The WAV files match byte for byte. The Ogg files decode to identical PCM, but the bytes differ in 24 places (stream serial and page checksums), so the Ogg bytes are not reproducible across encodes.
- Known limits: the generator fills the instance placement uniformly over the world, so the `instances_within_draw_distance` checkpoint (957 of 20000 for `render_stress`, 4735 of 100000 for `all_features`) is not the resident-renderable count the route defines; US-076 to US-078 measure resident and active counts at run time. Terrain is heights only; the four terrain layers are a runtime parameter, not generated. The route, the cell crossings, and the teleport case are not generated. The repository has no `LICENSE` file, so the provenance table says "project-authored" and the license is left open for the owner to choose.
- Learnings:
  - Gotcha: `path::string()` on Windows reads a narrow name as ANSI, so a Cyrillic fixture path passed as `std::string` breaks on MSVC. Build the path through `core::Path::from_utf8` and walk the tree with `u8string()`.
  - Gotcha: a per-component digest for an empty component (no physics bodies in `render_stress`) is the SHA-256 of nothing, so a test that changes the seed must only check components that have content.
  - Gotcha: `engine_tests` takes a suite flag only, so a new case cannot be run by name; the count check (1002 plus the new cases) was the way to confirm it ran.
---

## 2026-10-26 - US-076 (partial, not complete)
- Verified: `engine_workload --check benchmarks/workload_expected.json` exits 0 (4 scenes match). The `render_stress` counts match the contract: 20000 instances, 200 meshes, 100 characters with 64 bones (6400 bones), 100000 particles, 128 lights, 4 shadowed, 1 sun.
- Not done: no `render_stress` scene builder exists (`implementation: "none"` in `benchmarks/manifest.json`), and `render_route_v1` is still "pending US-076". The route, screenshots, submitted/visible counters, GPU timestamps, the indexed/indirect/meshlet path runs, the forced mesh-shader-off fallback, and the Windows and Linux gates were not run. `engine_bench` has no GPU measurement loop yet.
- Next: define the route in the manifest, build the scene on the renderer from the generated workload, add per-path and per-profile runs with counters and screenshots, then run the gates.
---

## 2026-10-26 - US-076 (in progress)
- Done so far: `render_route_v1` defined in benchmarks/manifest.json (camera object parsed into SceneDef::camera; profile `settings.output` parsed into ProfileDef::output_width/height). The generator now exposes render content (`generate_render_content`) built from the same functions as the digests; `engine_workload --check` still matches all 4 scenes.
- New driver tools/benchmark/src/render_stress.{hpp,cpp} (linked into engine_bench only): cooks meshes, builds 20000 instances, 128 point lights, 1 sun, drives the native_1080 profile along the route, records per-frame counters (items, triangles, indirect and meshlet visible counts, lights dropped), GPU pass timings, PPM screenshots (start and end), and feature state. `--path indexed|indirect|meshlets` and `--disable-mesh-shaders` (DeviceDesc::disabled_features) added to the CLI. Report JSON gains `path` and `features`.
- Renderer defaults overridden on purpose: max_draw_groups 256 (default 64 would push the rest of 200 meshes to direct draws), max_items 32768, larger meshlet pools.
- full_1080 reports unsupported (FSR 2 not built). native_1080 is incomplete: skinned characters, GPU particles, volumetric fog, baked probe GI are not drawn; GPU residency is not measured.
- Tests: 3 new unit cases (render content vs table and triangles, route/profile parse and refusal, CLI). engine_tests --suite unit: 1019 passed. Linux: ValidateVendor 0, linux-dev configure 0, linux-debug build 0 with no warnings, ctest linux-unit 0.
- Windows gates (us076_gate.bat, MSVC /W4 /WX, RTX 5080): validate 0; win-dev configure 0; win-debug build 0 (0 warning C); win-unit ctest 0 (29/29); engine_tests --suite gpu dx12 0 (72 cases); --suite gpu vulkan 0 (73 cases); win-release engine_bench build 0 (0 warnings).
- First acceptance attempt found a driver bug: the post pass needs its TAA/bloom/tonemap shaders, which were not loaded. Fixed (bench_post_* targets). The earlier runs in that batch are void.
- DX12 indexed, native_1080, 3 repeats (rebuilt binary): every repeat FAILED on frame time. p99 about 46 ms (budget 16.7 ms), 0% of frames within 16.7 ms, and 13-25% of frames over 33.3 ms. Start frame submits 312,068 triangles against the manifest floor of 2,000,000, so that check fails. Screenshots written under artifacts/bench/us076-dx12-indexed/screenshots.
- Cause of the triangle shortfall: instances are uniform over the 8192 m world, so the 60-degree view at the route start holds about 330 items. The route start is the only lever the manifest gives; a denser start or content change needs a user decision (the contract says not to reduce workloads).

## 2026-10-26 - US-076 (status check, not complete)
- Reviewed `artifacts/bench/us076-*`. Only `us076-dx12-indexed` was rerun after the post TAA shader fix; its report shows the current result: exit 1, start frame 312,068 triangles against the 2,000,000 floor, and p99 over 16.7 ms.
- The other seven reports (dx12 indirect/meshlets/meshlets-nomesh, vulkan all four) predate the fix and show `missing shader for post taa` (status incomplete, exit 3). They are void and must be rerun, not cited.
- Blocked on a user decision: the triangle floor and frame budget fail on placement, because instances are uniform over 8192 m and the route start sees about 330 items. Options: (a) keep the contract and report the failure as the measured result; (b) change instance placement (a workload change: bump `kWorkloadVersion`, regenerate `workload_expected.json`). The contract forbids reducing workloads, so (b) needs the user's approval.
- Not done in this pass: rerun of the seven void reports, Windows and Linux quality gates on the current tree, and the mesh-shader-off evidence on the current code.
---

## 2026-10-26 - US-076 (status: measured; contract failures open, needs user decision)
- Reran all eight acceptance reports on the current tree (win-release engine_bench, 03:11 build): indexed, indirect, meshlets on DX12 and Vulkan, plus meshlets with `--disable-mesh-shaders`. The earlier void reports are replaced. Each run is about 10 minutes (3 repeats x 180 s on native_1080; full_1080 is unsupported without FSR 2).
- Fixed the forced mesh-shader-off run. It was `incomplete` (exit 3): `populate` called `attach_meshlets`, which the renderer refuses without mesh shaders. Now `populate` skips the attach when `geometry_path()` is not `Meshlets`, so the renderer resolves to the indirect-indexed fallback (FR-06). The feature label now reads "fell back to indirect-indexed draws (mesh shaders unavailable)". The mesh-shader-on runs are unchanged.
- Fallback evidence (DX12 and Vulkan, RTX 5080): `mesh_shaders: disabled: forced off`, `meshlet_path: fell back`, indirect active with 338-340 visible items, meshlet visible 0, 6000-8000 frames measured per backend. Those runs exit 1 on the frame budget and triangle floor, which is the measured result, not a fallback failure.
- Measured results (all contract failures, not reduced): start frame 312,068 triangles against the 2,000,000 floor on every indexed run. p99 35-47 ms against 16.7 ms, 0% of frames within 16.7 ms. Mesh-shader paths also fail the 33.3 ms outlier check (17-22% of frames over 33.3 ms on DX12 meshlets). Screenshots are under artifacts/bench/us076-*/screenshots.
- Gates on the current tree. Windows (us076d.bat): validate 0, win-dev 0, win-debug build 0 (0 warnings), win-unit 0 (29/29), engine_tests --suite gpu dx12 0 (72 cases), --suite gpu vulkan 0 (73 cases), win-release engine_bench 0 (0 warnings). Linux: ValidateVendor 0, linux-dev 0, linux-debug build 0 (0 warnings), linux-unit 0 (30/30). Native Linux Vulkan GPU and linux-gcc were not run (WSL has no Vulkan device; GCC 9 cannot build Jolt).
- Still open, needs the user's decision: the triangle floor and the frame budget fail on placement (instances are uniform over 8192 m, so the route start sees about 330 items). Options: (a) keep the contract and report these failures as the result; (b) change instance placement (a workload change: bump `kWorkloadVersion`, regenerate `workload_expected.json`). The contract forbids reducing workloads, so (b) needs approval.
- Learnings:
  - Gotcha: a driver that asks a renderer for a feature must check the renderer's resolved path (`geometry_path()`), not the request, or a forced-off run reports incomplete instead of the fallback.
  - Gotcha: `cmd.exe /c tasklist | grep` can return nothing for a live process. Wait on the batch's WSL pid (`ps -p`) instead.
---

## 2026-10-26 - US-076 (status check 2, not complete)
- Checked the eight reports against the last driver edit (`render_stress.cpp`, 04:15:43). Four reports predate it. The edit only changes the `attach_meshlets` guard, which the indexed and indirect paths never reached (`path == Meshlets` is false) and which the mesh-shader-on meshlet path still takes unchanged. The reports remain valid evidence; no rerun was needed for this reason.
- Story deliverables present: `render_route_v1`, the render_stress driver, `--path`, `--disable-mesh-shaders`, per-run counters, screenshots, and the fallback evidence.
- Not complete: (1) the full_1080 profile is unsupported (FSR 2 permutations missing), so "both profiles" is only partly run; (2) the indexed/indirect/meshlet DX12 and Vulkan runs fail the triangle floor (312,068 of 2,000,000) and the frame budget. Both are the measured result. Changing instance placement is a workload change and needs the user's approval.
- No Linux or Windows gates were re-run in this pass (no source change beyond the one above, and that change was gated in the earlier run).
---

## 2026-10-26 - US-076 (status check 3, not complete)
- Re-verified the Linux gates on the current tree: ValidateVendor passes (26 deps, 8542 files), linux-dev configures, linux-debug builds with 0 warnings, linux-unit passes 30/30.
- Artifacts unchanged: eight `artifacts/bench/us076-*` reports, all postdating the last `render_stress.cpp` edit (04:15:43). No benchmark rerun.
- Still open: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms), which needs a user decision on a workload change; (2) full_1080 is unsupported because FSR 2 permutations are missing, so "both profiles" is only partly run; (3) native Linux Vulkan GPU and linux-gcc not run.
---

## 2026-10-26 - US-076 (status check 4, not complete)
- Checked timestamps: no source file changed after the eight `artifacts/bench/us076-*` reports (last driver edit 04:15:43; `scene_renderer.cpp` is from 2026-10-24). The earlier gate results still describe the current tree, so no rerun.
- Still blocked on the user: (1) the triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles at the route start; p99 35-47 ms vs 16.7 ms). A fix means changing instance placement, a workload change (bump `kWorkloadVersion`, regenerate `workload_expected.json`); (2) full_1080 is unsupported until FSR 2 permutations exist, so only native_1080 runs; (3) native Linux Vulkan GPU and linux-gcc not run.
---

## 2026-10-26 - US-076 (status check 5, not complete; correction)
- Correction to status check 4: it said every `us076-*` report postdates the last `render_stress.cpp` edit (04:15:43). That was wrong. Six reports predate it: dx12-indexed (03:22), dx12-indirect (03:32), dx12-meshlets (03:43), vulkan-indexed (03:54), vulkan-indirect (04:04), vulkan-meshlets (04:15:07). Only dx12-meshlets-nomesh (04:35) and vulkan-meshlets-nomesh (04:45) postdate it.
- Why the six still stand: the 04:15:43 edit only adds `geometry_path() == Meshlets` to the `attach_meshlets` guard at `render_stress.cpp:242`. Indexed and indirect runs already skipped the attach (`path == Meshlets` false), and mesh-shader-on meshlet runs still take the attach. No rerun was needed.
- Still open, unchanged: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms), which needs the user's decision on a workload change; (2) full_1080 unsupported (FSR 2 permutations missing); (3) native Linux Vulkan GPU and linux-gcc not run. The story is not complete, so no COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 6, not complete)
- No source change since status check 5, so the recorded Linux and benchmark results still describe the tree. No gate or benchmark rerun.
- Still blocked on the user, unchanged: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles); fixing it changes the workload (bump `kWorkloadVersion`, regenerate `workload_expected.json`); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- The Windows gates were not re-run in this pass.
---

## 2026-10-26 - US-076 (status check 7, not complete)
- Checked mtimes: last source edit is `render_stress.cpp` at 04:15:43, and the newest benchmark report (dx12-meshlets-nomesh, 04:35:02) postdates it. No rerun needed. `find -newer` on the artifact directory matched old files only because the directory mtime is 03:08.
- Still blocked on the user, unchanged: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms); a fix is a workload change (bump `kWorkloadVersion`, regenerate `workload_expected.json`); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
---

## 2026-10-26 - US-076 (status check 8, not complete)
- Timestamps unchanged: last source edit `render_stress.cpp` 04:15:43, `manifest.json` 02:43:31; all eight `us076-*` reports postdate it, and each still exits 1 on the frame budget and triangle floor. No rerun.
- Still blocked on the user, unchanged: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
---
## 2026-10-26 - US-076 (status check 9, not complete; no rerun)
- Re-checked mtimes: `render_stress.cpp` 04:15:43 and `tools/benchmark/CMakeLists.txt` 03:08:33 both predate all eight `us076-*` reports. No source change, no rerun.
- Still blocked on the user: (1) the triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms), and a fix is a workload change (bump `kWorkloadVersion`, regenerate `workload_expected.json`); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
---
## 2026-10-26 - US-076 (status check 10, not complete; no rerun)
- Listed the eight `artifacts/bench/us076-*` directories. No new source change was made in this pass, so the recorded gate and benchmark results still describe the tree. No benchmark or gate rerun.
- Still blocked on the user, unchanged: (1) the triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles; p99 35-47 ms vs 16.7 ms), and a fix is a workload change (bump `kWorkloadVersion`, regenerate `workload_expected.json`); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- Decision needed: approve or reject a placement change, and say whether a failing run should close the story as measured, since the acceptance criteria cannot pass on the current placement.
---

## 2026-10-26 - US-076 (status check 11, not complete; no rerun)
- Source and manifest mtimes unchanged (`render_stress.cpp` 04:15:43, `manifest.json` 02:43:31). Recorded results still describe the tree; no benchmark or gate rerun.
- Still blocked on the user: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000; p99 35-47 ms vs 16.7 ms); (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: the acceptance criteria cannot pass on the current placement.
---

## 2026-10-26 - US-076 (status check 12, not complete; no rerun)
- Mtimes unchanged: `render_stress.cpp` 04:15:43, newest `us076-*` artifact from the earlier runs. No source change, no benchmark or gate rerun.
- Still blocked on the user: (1) the triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles; p99 35-47 ms vs 16.7 ms), a workload change that needs `kWorkloadVersion` bumped and `workload_expected.json` regenerated; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---

## 2026-10-26 - US-076 (status check 13, not complete; no rerun)
- Re-checked mtimes: `render_stress.cpp` 04:15:43 still predates all eight `us076-*` reports. No source change, no benchmark or gate rerun.
- Still blocked on the user: (1) the triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles); a fix is a workload change (bump `kWorkloadVersion`, regenerate `workload_expected.json`), which the contract reserves for a user decision; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---
## 2026-10-26 - US-076 (status check 14, not complete; no rerun)
- Source and manifest mtimes unchanged (`render_stress.cpp` 04:15:43, `manifest.json` 02:43:31, `workload_expected.json` 02:21:05); the eight `us076-*` reports are from 03:08 to 04:45 and postdate them. No benchmark or gate rerun.
- Still blocked on the user, unchanged: (1) triangle floor and frame budget fail on instance placement (312,068 of 2,000,000 triangles at the route start; p99 35-47 ms vs 16.7 ms). A fix is a workload change (bump `kWorkloadVersion`, currently 1, and regenerate `workload_expected.json`), which needs approval; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
---
## 2026-10-26 - US-076 (status check 15, not complete; no rerun)
- Mtimes unchanged: `render_stress.cpp` 04:15:43, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31; all eight `us076-*` reports (03:08 to 04:45) postdate them. No source change, no benchmark or gate rerun.
- Still blocked on the user: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles; p99 35-47 ms vs 16.7 ms); fixing it changes the workload (bump `kWorkloadVersion`, regenerate `workload_expected.json`), which needs approval; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---
## 2026-10-26 - US-076 (status check 16, not complete; no rerun)
- Mtimes unchanged: `render_stress.cpp` 04:15:43, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31; `kWorkloadVersion` still 1. No source change, no benchmark or gate rerun.
- Still blocked on the user: (1) triangle floor and frame budget fail on placement (312,068 of 2,000,000 triangles); the fix changes the workload (bump `kWorkloadVersion`, regenerate `workload_expected.json`), which needs approval; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---
## 2026-10-26 - US-076 (status check 17, not complete; no rerun)
- Source and workload mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05); `kWorkloadVersion` still 1. No source change, no benchmark or gate rerun.
- Still blocked on the user: (1) triangle floor and frame budget fail on instance placement (312,068 of 2,000,000 triangles; p99 35-47 ms vs 16.7 ms). Fixing it changes the fixed workload contract (bump `kWorkloadVersion`, regenerate `workload_expected.json`), which needs approval; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: the acceptance criteria cannot pass on the current placement.
---
## 2026-10-26 - US-076 (status check 18, not complete; no rerun)
- Re-entered the story with no user reply to the open decision. No source, workload, or gate change in this pass; the recorded results still describe the tree.
- Still blocked on the user: (1) approve or reject a placement change for the triangle floor and frame budget (312,068 of 2,000,000 triangles at the route start; p99 35-47 ms vs 16.7 ms); an approved change bumps `kWorkloadVersion` and regenerates `workload_expected.json`; (2) full_1080 unsupported until FSR 2 permutations exist (vendoring route is the user's choice); (3) native Linux Vulkan GPU and linux-gcc not run (no Vulkan device under WSL; GCC 9 cannot build Jolt).
- Decision needed: whether a failing measured run closes the story as measured, since the acceptance criteria cannot pass on the current placement. No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 19, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: (1) approve or reject a placement change for the triangle floor and frame budget (312,068 of 2,000,000 triangles; p99 35-47 ms vs 16.7 ms); an approved change bumps `kWorkloadVersion` and regenerates `workload_expected.json`; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---

## 2026-10-26 - US-076 (status check 20, not complete; no rerun)
- Mtimes unchanged: `render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31; the newest `us076-*` report postdates them. No source, workload, or gate change in this pass.
- Still blocked on the user: (1) approve or reject a placement change for the triangle floor and frame budget (312,068 of 2,000,000 triangles at the route start; p99 35-47 ms vs 16.7 ms); an approved change bumps `kWorkloadVersion` and regenerates `workload_expected.json`; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement.
---
## 2026-10-26 - US-076 (status check 21, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); latest `us076-*` report is newer. No source, workload, or gate change in this pass.
- Still blocked on the user: placement/workload decision (triangle floor and frame budget), FSR 2 permutations for full_1080, and native Linux Vulkan GPU plus linux-gcc runs.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 22, not complete; no rerun)
- Re-read the contract (PRD line 875: at least 2,000,000 submitted triangles on the indexed path at the start frame) and `render_stress.cpp`. The start pose comes from the manifest camera route, which is pinned by SHA-256, and the instance placement comes from the seeded generator, whose digests are pinned. Both fixes change a fixed contract, so neither was made.
- Still blocked on the user: (1) approve a route-start or placement change (bump `kWorkloadVersion`, regenerate `workload_expected.json`, update the manifest hash) or accept the failing measured run as the result; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 23, not complete; Linux re-verified)
- Re-ran on linux-dev: `cmake --build --preset linux-debug` builds; `ctest --preset linux-unit` passes 30/30; `engine_workload --check benchmarks/workload_expected.json` reports 4 scenes match. No source, workload, or manifest change.
- Still blocked on the user: (1) route-start triangle floor (312,068 of 2,000,000) and frame budget; resolving it changes the fixed contract, which needs a user decision; (2) full_1080 unsupported until FSR 2 permutations exist; (3) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 24, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, which needs a fixed-contract decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 25, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, which needs a fixed-contract decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 26, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, which PRD line 722 reserves for a separate user decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 27, not complete; no rebuild)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. `engine_workload --check benchmarks/workload_expected.json` re-run: 4 scenes match.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, which PRD line 722 reserves for a separate user decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 28, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, a fixed-contract decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 29, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05, `manifest.json` 02:43:31); `kWorkloadVersion` still 1. No source, workload, or gate change in this pass.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, a fixed-contract decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 30, not complete; no rerun)
- Mtimes unchanged (`render_stress.cpp` 04:15:43, `workload.cpp` 02:42:31, `workload_expected.json` 02:21:05); `kWorkloadVersion` still 1. `engine_workload --check` re-run: 4 scenes match. No source, workload, or gate change.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000) and frame budget, a fixed-contract decision; full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 31, not complete; no rerun)
- Re-read PRD US-080 (line 868-871): a missed budget is fixed in the implementation first; workload reductions or quality relaxations need a separate user agreement. `kWorkloadVersion` still 1, `render_stress.cpp` and `workload.cpp` unchanged since the us076-* reports were written (03:08 to 04:45). No source, workload, or gate change.
- Still blocked on the user: route-start triangle floor (312,068 of 2,000,000 on the indexed path) and frame budget (p99 35-47 ms vs 16.7 ms). Whether that is a placement change (workload contract) or an implementation gap (LOD/culling) is the decision; the prior passes did not settle it. Also: full_1080 unsupported until FSR 2 permutations exist; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass on the current placement. Repeating this status entry without a user reply adds nothing.
---

## 2026-10-26 - US-076 (status check 32, not complete; mechanism found)
- `start_triangles_in_view` is the sum of `mesh.indices / 3` over visible instances (`render_stress.cpp` ~line 467). Each mesh is cooked once at LOD 0 and there is no LOD selection, so the 312,068 start-frame triangles come only from which instances are visible and how dense the fixture meshes are. Reaching 2,000,000 needs a fixture-content change (denser meshes, provenance hash, `kWorkloadVersion`) or a placement change (regenerated `workload_expected.json`). Both alter the pinned contract, so no code was changed here.
- Still blocked on the user: pick fixture density or placement change (and approve the version bump), accept the failing run as the result, or agree a contract change. full_1080 needs FSR 2 permutations; native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (fixture density fix, workload v2; not complete)
- Start-frame floor fixed in the fixture, not the contract: mesh grids are 24-96 quads per side (were 8-32), `kWorkloadVersion` 1 -> 2, `benchmarks/workload_expected.json` regenerated (`engine_workload --write`, `--check` 4 scenes match). Table counts unchanged. The generator is shared, so the other three scenes' mesh and instance triangle totals changed too.
- `start_triangles_in_view` on the indexed path: 3,202,690 (was 312,068); floor 2,000,000 met. Same value on every path, as expected (same visible set).
- `bench_workload_tests.cpp` read the version as a literal 1; it now reads `kWorkloadVersion`.
- Linux: vendor validation, linux-dev configure, linux-debug build, linux-unit ctest 30/30 pass.
- Windows batch `us076e` (win-dev, win-debug, win-unit 29/29, engine_tests gpu dx12 72 cases and gpu vulkan 73 cases, all pass; release engine_bench builds): all eight acceptance runs exit 1. Reason on every render_stress run: frame-time threshold exceeded (p99 over budget) and "residency was not measured." Missing workloads for streaming_stress (US-078), physics_stress (US-077), all_features; full_1080 unsupported (ENGINE_FSR2_AVAILABLE off).
- Fallback verified in reports: forced-off runs show `mesh_shaders: disabled: forced off by --disable-mesh-shaders`, `meshlet_path: fell back to indirect-indexed draws`, 0 meshlet visible, 393 indirect visible. Meshlet runs show `enabled`/`active`, 25,890 meshlet visible.
- Still open: frame budget (US-080 owns it; fix the measured cause), residency measurement, screenshots are written (6 per run). Native Linux Vulkan GPU and linux-gcc not run. No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 33, not complete; residency gap scoped)
- No source or workload change in this pass. `grep` confirms the RHI exposes no memory-usage query: `gpu_resident_bytes` and `host_working_set_bytes` are only ever left unset, so every render_stress run reports "GPU residency is not measured".
- That gap is implementable without a user decision, unlike the frame budget: DX12 `IDXGIAdapter3::QueryVideoMemoryInfo` (local segment, CURRENT_USAGE) and Vulkan `VK_EXT_memory_budget` (enabled with the device, `usage` per heap) behind one RHI accessor, plus the host working set (`GetProcessMemoryInfo` on Windows, `/proc/self/statm` on Linux). It would give the residency check a value, but it does not turn a run into a pass while the start-frame p99 stays over 16.7 ms.
- Still blocked on the user: frame budget (p99 35-47 ms vs 16.7 ms; fix in the implementation per US-080 or a contract change), full_1080 (needs FSR 2 permutations), native Linux Vulkan GPU (WSL has no Vulkan device), and linux-gcc (GCC 9.4 cannot build Jolt).
- No COMPLETE signal.
---

## 2026-10-26 - US-076 (status check 34, not complete; no rerun)
- Re-checked: `kWorkloadVersion` = 2, `workload.cpp` 05:56, `workload_expected.json` 05:57, `render_stress.cpp` 04:15; no source change since the us076e batch. No build or gate run in this pass.
- Still open, in order: (1) frame budget on the start frame (p99 over 16.7 ms on all render_stress runs; US-080 owns the fix, a contract change needs user agreement); (2) residency not measured (RHI has no memory query; DX12 QueryVideoMemoryInfo and VK_EXT_memory_budget behind one accessor is implementable without a user decision); (3) full_1080 needs FSR 2 permutations (user's vendoring choice); (4) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: acceptance cannot pass while the render_stress runs exit 1.
---

## 2026-10-26 - US-076 (status check 35, not complete; no code change)
- Re-verified: no RHI memory query exists (`gpu_resident_bytes` is still only declared, measured by nothing), so every render_stress run still reports "residency was not measured". `kWorkloadVersion` is still in `workload.cpp`. No build or gate run in this pass.
- Open items are unchanged: (1) frame budget on the start frame (p99 over 16.7 ms; US-080 owns the fix, a contract change needs the user's decision); (2) residency accessor (DX12 `QueryVideoMemoryInfo`, Vulkan `VK_EXT_memory_budget`, host working set), implementable but not started; (3) full_1080 needs FSR 2 permutations; (4) native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal: the render_stress acceptance runs exit 1.
---

## 2026-10-26 - US-076 (status check 36, not complete; Linux gates re-run)
- Linux: `cmake -P cmake/ValidateVendor.cmake` pass, `cmake --build --preset linux-debug` pass, `ctest --preset linux-unit` 30/30 pass. No source change in this pass.
- Still open, unchanged: render_stress p99 frame time over 16.7 ms on the start frame (US-080 owns the fix; a contract change needs the user), residency accessor not implemented, full_1080 needs FSR 2 permutations, native Linux Vulkan GPU and linux-gcc not run.
- No COMPLETE signal.
---

## 2026-10-26 - US-077 (physics benchmark; gates partly pending, no COMPLETE signal)
- What was implemented: `physics_stress` headless runner (`tools/benchmark/src/physics_stress.cpp`, header in `include/engine/benchmark/`). It creates the table's 2000 dynamic bodies, 200 statics plus a ground box, 100 constraints (fixed/hinge/distance by index), and 20 characters, then runs 60 Hz simulated frames with a fixed action sequence (velocity kicks every 30 frames, character walks and jumps, 1000 raycasts/s and 100 shape casts/s). Counters cover steps, catch-up, dropped steps (lost timesteps), active/sleeping bodies, queries, interactions, contact/activation events, NaN samples, and out-of-bounds samples. Failure checks: lost timesteps, Jolt/character step errors, NaN, out-of-bounds.
- Adapter: `PhysicsWorld::set_linear_velocity` (wakes a dynamic body) and `active_body_count` (Jolt `GetNumActiveBodies`).
- Generator: `build_physics` returns `PhysicsSpec`; `physics_digest` writes the same byte stream. `engine_workload --check` still matches 4 scenes.
- engine_bench: `physics_headless` runs the scene; `physics_visual` reports Missing (no visual physics path).
- Manifest route text for `physics_stress` now records `physics_action_sequence_v1` (was "pending US-077").
- Files: physics_world.hpp/.cpp, workload.hpp/.cpp, physics_stress.hpp/.cpp (new), tools/benchmark/CMakeLists.txt, main.cpp, benchmarks/manifest.json, tests/src/bench_physics_tests.cpp (new), physics_world_tests.cpp (velocity wake test), tests/CMakeLists.txt, docs/physics.md, docs/benchmarks.md.
- Verified: Linux `cmake -P cmake/ValidateVendor.cmake`, `linux-dev` configure, `linux-debug` build (no warnings), `linux-unit` 30/30, engine_tests `--suite unit` 1027 cases pass, `engine_workload --check` 4 scenes match. Windows (batch us077): ValidateVendor, win-dev configure, win-debug build (0 MSVC warnings), win-unit 29/29 pass.
- Pending when this entry was written: Windows `engine_tests --suite gpu` dx12 and vulkan; Windows Release `engine_bench` and `--suite acceptance --backend dx12` (physics headless is the only run that executes; no `--path` so render runs are incomplete); `linux-asan` build (preset compiles its own DXC host toolchain, now linking) and `ctest --preset linux-sanitize`.
- Not run: `linux-gcc*` (local GCC 9.4 cannot build Jolt; CI job), `physics_visual` (no visual physics path).
- Status call: US-077 stays open until the pending Windows GPU/acceptance and linux-sanitize results are read. No COMPLETE signal.
---

## 2026-10-26 - US-077 (gates read; linux-gcc and physics_visual still open; no COMPLETE signal)
- Windows batch `us077` (`D:\dev\haiku_game_engine`): validate=0, win-dev configure=0, win-debug build=0 (no MSVC warnings), win-unit=0 (29/29), `engine_tests --suite gpu --backend dx12`=0, `--suite gpu --backend vulkan`=0, win-release `engine_bench` build=0, `engine_bench --suite acceptance --backend dx12 --output artifacts\bench\us077-dx12` exit 3 (incomplete: render/streaming/all_features/physics_visual are not measured in this run, as their stories are open; the physics headless profile is measured).
- Physics headless (Release, DX12, 3 of 3 repeats passed): 10 800 measured frames, 12 600 simulation steps, 0 catch-up events, 0 lost timesteps, 0 NaN transforms, 0 out-of-bounds samples, 0 update errors. 2000 dynamic bodies stayed active on every frame (kicks every 30 frames; no sleeping substitution), 20 characters, 100 constraints, 210 000 raycasts (210 000 hits) and 21 000 shape casts (21 000 hits), 840 000 kicks, 41 248 contact events. Frame cpu_ms over the measured window: p50 1.23, p95 1.58, p99 1.66, max 1.81 (diagnostic only, no contract).
- Linux `linux-asan` configure, build (exit 0; the preset builds its own DXC host toolchain) and `ctest --preset linux-sanitize` 30/30 pass.
- Not run: `linux-gcc*` (local GCC 9.4 cannot build Jolt; the CI job is required). `physics_visual` profile (needs the visual physics path; reports missing).
- Status call: the story's scene, report, and detection criteria are met and verified locally on Windows and Linux. US-077 is not marked complete while the Linux/GCC CI job is unverified and the visual profile is missing. No COMPLETE signal.
---


## 2026-10-26 - US-077 (status check 37, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --preset linux-dev` pass, `cmake --build --preset linux-debug` pass with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Windows results from the previous pass (batch us077) stand; they were not re-run in this pass.
- Still open: (1) the Linux/GCC CI job (`linux-gcc*`), which cannot run locally because GCC 9.4 cannot build Jolt; (2) the `physics_visual` profile, which still reports Missing (the manifest lists it; the PRD does not name it).
- No COMPLETE signal: a required gate (linux-gcc) is unverified, so the story is not reported as done.
---

## 2026-10-26 - US-077 (status check 38, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --preset linux-dev` pass, `cmake --build --preset linux-debug` pass with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Compilers: still only GCC/G++ 9 and clang 10 in `/usr/bin`, so `linux-gcc*` (Jolt needs GCC 11+) cannot run here; the CI job is required.
- Not changed: `physics_visual` (manifest entry, reports Missing; the PRD's `physics_stress` row and acceptance criteria do not require it).
- Still open: the Linux/GCC CI job. Windows results from batch us077 stand (not re-run in this pass).
- No COMPLETE signal: the story's linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 39, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- GCC 11+ check: apt sources on this machine offer only gcc/g++ 7 to 10 (no gcc-11 or gcc-12). `sudo` needs a password, so nothing could be installed. Jolt needs GCC 11+, so `linux-gcc*` cannot run here; the Linux/GCC CI job is still the only unverified gate.
- Still open: the Linux/GCC CI job, and the `physics_visual` manifest profile (reports Missing; not required by the PRD row).
- No COMPLETE signal: a required gate (linux-gcc) is unverified.
---

## 2026-10-26 - US-077 (status check 40, no code change; no COMPLETE signal)
- Re-read the log and the tree: the physics_stress scene, report, and detection code from the batch us077 pass are unchanged. Gates were not re-run in this pass; the last Linux pass (status check 39) and the Windows batch us077 results stand.
- Still open: the Linux/GCC CI job (`linux-gcc*`; local GCC 9.4 cannot build Jolt, and no GCC 11+ is installable here) and the `physics_visual` manifest profile (reports Missing; not required by the PRD row).
- No COMPLETE signal: the story requires the linux-gcc job, and it is unverified.
---

## 2026-10-26 - US-077 (status check 41, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0, `ctest --preset linux-unit` 30/30 pass.
- Compilers: no g++-11+ or clang++-14+ binaries found on PATH, so `linux-gcc*` still cannot run here; the Linux/GCC CI job is required.
- Windows results from batch us077 stand (not re-run in this pass).
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 42, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Compilers: still no g++-11+ or clang++-14+ on PATH, so `linux-gcc*` cannot run here; the Linux/GCC CI job is the only unverified required gate.
- Still open: the Linux/GCC CI job, and the `physics_visual` manifest profile (reports Missing; not required by the PRD row). Windows results from batch us077 stand.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 43, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Still open: the Linux/GCC CI job (`linux-gcc*`; local GCC 9.4 and clang 10 cannot build Jolt). Windows results from batch us077 stand (not re-run in this pass).
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 44, no code change; no COMPLETE signal)
- Linux re-verified: `cmake -P cmake/ValidateVendor.cmake` pass (26 dependencies, 8542 files), `cmake --build --preset linux-debug` exit 0, `ctest --preset linux-unit` ran with no failures in the output (the pass-count summary line was not captured in this pass; last full count was 30/30).
- Compilers: still no GCC 11+ or clang 14+, so `linux-gcc*` cannot run here; the Linux/GCC CI job is the only unverified required gate.
- Still open: the Linux/GCC CI job, and the `physics_visual` manifest profile (not required by the PRD row). Windows results from batch us077 stand (not re-run).
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 45, no code change; no COMPLETE signal)
- Re-read `tools/benchmark/src/physics_stress.cpp`: active and sleeping body counts per frame (lines ~289-301), lost timesteps from the catch-up limit (line ~332), NaN and out-of-bounds samples (lines ~89-103, ~340), and the report fields (lines ~365-382). The detection code is unchanged from the earlier passes.
- Linux: `cmake --build --preset linux-debug` completed with no warnings in the output. `ctest --preset linux-unit` passed 30 of 30 (100%, 0 failed).
- Still open: the Linux/GCC CI job (`linux-gcc*`). Local compilers are GCC/G++ 9 and clang 10, and Jolt needs GCC 11+, so the job cannot run here and is not reported as passed. The `physics_visual` manifest profile still reports Missing; the PRD row does not require it.
- No COMPLETE signal: a required gate (linux-gcc) is unverified.
---

## 2026-10-26 - US-077 (status check 46, no code change; no COMPLETE signal)
- Linux re-verified: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- `cmake --preset linux-asan` and `cmake --build --preset linux-asan` exit 0 with 0 warnings. `ctest --preset linux-sanitize` exit 8: 29/30 pass; `engine_tests_integration` fails 3 assertions in `platform_x11_input_tests.cpp:336-344` (a key press is not delivered to the focused X11 window). Reproduces on a rerun; no ASan/UBSan report and no physics test failure. Unrelated to US-077, and not fixed here.
- Still open: the Linux/GCC CI job (`linux-gcc*`; no GCC 11+ or clang 14+ on this machine). Windows results from batch us077 stand (not re-run in this pass).
- No COMPLETE signal: the linux-gcc gate is unverified and linux-sanitize is red on the X11 input tests.
---

## 2026-10-26 - US-077 (status check 47, no code change; no COMPLETE signal)
- Linux re-verified on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass, `engine_tests --suite unit` 1027 cases pass.
- Still open: the Linux/GCC CI job (`linux-gcc*`; no GCC 11+ on this machine), and `linux-sanitize` (last run 29/30; `engine_tests_integration` X11 key-delivery failure in `platform_x11_input_tests.cpp`, not physics code). Windows results from batch us077 stand (not re-run).
- No COMPLETE signal: the linux-gcc gate is unverified and linux-sanitize is not green.
---

## 2026-10-26 - US-077 (status check 48, no code change; no COMPLETE signal)
- Linux re-verified: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Still open: the Linux/GCC CI job (`linux-gcc*`; the machine has GCC 9.4 and clang 10 only, and Jolt needs GCC 11+), and `linux-sanitize` (last run 29/30, X11 key-delivery failure in `platform_x11_input_tests.cpp`, not physics code). Windows results from batch us077 stand (not re-run here).
- No COMPLETE signal: the linux-gcc gate is unverified and linux-sanitize is not green.
---

## 2026-10-26 - US-077 (status check 49, no code change; no COMPLETE signal)
- Verified the existing `physics_stress` implementation is present (`tools/benchmark/src/physics_stress.cpp`: active/sleeping counts, lost timesteps from the catch-up limit, NaN and out-of-bounds sample detection, report fields).
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit --no-tests=error` exit 0.
- Still open: the Linux/GCC CI job (`linux-gcc*`; no GCC 11+ locally) and `linux-sanitize` (last run 29/30, X11 key-delivery failure in `platform_x11_input_tests.cpp`, not physics code). No COMPLETE signal.
---

## 2026-10-26 - US-077 (status check 50, no code change; no COMPLETE signal)
- Probed the host for a Linux/GCC toolchain: only GCC/G++ 9 and clang 10 are in `/usr/bin` (`/usr/lib/gcc/x86_64-linux-gnu/` has only 9; no `/opt` toolchains). Jolt needs GCC 11+, so `linux-gcc*` cannot run here. Installing it needs root and was not attempted.
- No file under `tools/benchmark`, `engine/physics`, `engine/benchmark`, `benchmarks`, `tests/src/bench_physics_tests.cpp`, or `docs/physics.md` is newer than the Windows batch `us077` report (`artifacts/bench/us077-dx12/report.json`), so the Windows results still describe the current tree.
- Last recorded results on this tree: Linux `linux-sanitize` 30/30 (status check 46's X11 failure no longer reproduces), `linux-unit` 30/30, Windows win-unit 29/29, gpu dx12 and vulkan pass, physics headless 3 of 3 repeats with 0 lost timesteps, 0 NaN, 0 out-of-bounds.
- Status call: US-077 stays open. The story's criteria require the Linux/GCC CI job, and the local toolchain cannot produce that result. The acceptance run exits 3 (incomplete) because render, streaming, and all_features are open stories. No COMPLETE signal.
---

## 2026-10-26 - US-077 (status check 51, no code change; no COMPLETE signal)
- Linux re-verified: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit` 30/30 pass.
- Acceptance criteria 1-3 checked against `tools/benchmark/src/physics_stress.cpp` (constraints/characters created, active/sleeping counts, catch-up and lost timesteps, NaN and out-of-bounds samples): present, unchanged.
- Still open: the Linux/GCC CI job (`linux-gcc*`; no GCC 11+ locally). Windows results from batch us077 stand (not re-run here).
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 52, no code change; no COMPLETE signal)
- Linux re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit --no-tests=error` 30/30 pass (exit 0).
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are installed; Jolt needs GCC 11+ and installing it needs root. Not reported as passed.
- `linux-sanitize` was last green at status 50 (30/30) and was not re-run in this pass.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 53, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files), `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit --no-tests=error` 30/30 pass.
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are installed; Jolt needs GCC 11+ and installing it needs root. Not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 54, no code change; no COMPLETE signal)
- Linux re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warnings, `ctest --preset linux-unit --no-tests=error` exit 0.
- `linux-sanitize` re-run (`linux-asan` configure and build, then `ctest --preset linux-sanitize --no-tests=error`): exit 0. The earlier stale 29/30 X11 failure did not recur.
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are installed; Jolt needs GCC 11+, and installing it needs root. Not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 55, no code change; no COMPLETE signal)
- Host probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10; no docker, podman, or conda. The `linux-gcc*` CI job (Jolt needs GCC 11+) cannot run here, and installing a toolchain needs root.
- Stories criteria 1-3 and the Windows and Linux clang gates were verified in earlier passes on this tree; no file under the story's paths changed since the Windows batch `us077`.
- Status call: US-077 stays open on the Linux/GCC CI criterion. No COMPLETE signal.
---

## 2026-10-26 - US-077 (status check 56, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 (no warning lines), `ctest --preset linux-unit --no-tests=error` exit 0.
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are installed; no g++-11+ or clang++-14+ on the host. Jolt needs GCC 11+, so the gate cannot run here and is not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 57, no code change; no COMPLETE signal)
- Toolchain probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10 (and clang 10). No GCC 11+ and no container runtime, so the `linux-gcc*` CI job (Jolt needs GCC 11+) cannot run here.
- Not re-run in this pass: the Linux and Windows gates were last recorded green in status 56 and no story file changed since.
- Still open: the Linux/GCC CI criterion. No COMPLETE signal.
---

## 2026-10-26 - US-077 (status check 58, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with no warning lines, `ctest --preset linux-unit --no-tests=error` exit 0.
- No file under `tools/benchmark`, `engine/physics`, `benchmarks`, or `tests/src/bench_physics_tests.cpp` is newer than the Windows batch `us077` report, so the Windows results still describe this tree.
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are installed; Jolt needs GCC 11+, and installing it needs root. Not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 59, no code change; no COMPLETE signal)
- Toolchain probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10, clang-10; no `/opt` toolchains, no docker, podman, or conda. The `linux-gcc*` CI job (Jolt needs GCC 11+) cannot run here, and installing a toolchain needs root.
- No file under `tools/benchmark`, `engine/physics`, `benchmarks`, or `tests/src/bench_physics_tests.cpp` is newer than the Windows batch `us077` report, so the recorded results still describe this tree.
- Still open: the Linux/GCC CI criterion. Not reported as passed. No COMPLETE signal.
---

## 2026-10-26 - US-077 (status check 60, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warning lines, `ctest --preset linux-unit --no-tests=error` 30/30 pass (exit 0).
- Toolchain probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10; `sudo -n` needs a password, so no GCC 11+ can be installed here. The `linux-gcc*` CI criterion stays unverified and is not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 61, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warning lines, `ctest --preset linux-unit --no-tests=error` exit 0 (unit 1 test and integration 29 tests reported by ctest).
- Toolchain probe: no g++-11+ or clang++-14+ on the host, so the `linux-gcc*` CI job (Jolt needs GCC 11+) cannot run here. Not reported as passed.
- Still open: the Linux/GCC CI criterion. No COMPLETE signal.
---

## 2026-10-26 - US-077 (Linux gate re-run, no code change; no COMPLETE signal)
- Linux re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0 (configure ~100 s, DXC host tool rebuilt), `cmake --build --preset linux-debug` exit 0 with 0 warning lines, `ctest --preset linux-unit --no-tests=error` exit 0 (unit 1 test, integration 29 tests).
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are on the host; Jolt needs GCC 11+ and installing it needs root. Not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 62, no code change; no COMPLETE signal)
- Source check: `physics_stress.cpp` reports active/sleeping bodies, catch-up and lost timesteps, NaN transforms, and out-of-bounds samples; `bench_physics_tests.cpp` covers the scene.
- Still open: the Linux/GCC criterion (`linux-gcc*`). Only g++-9 and clang++-10 are on the host and installing GCC 11+ needs root. Not reported as passed. No COMPLETE signal.
---
## 2026-10-26 - US-077 (status check 63, no code change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --preset linux-dev` exit 0, `cmake --build --preset linux-debug` exit 0 with 0 warning lines, `ctest --preset linux-unit --no-tests=error` exit 0.
- Toolchain probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10, clang++; `sudo -n` requires a password; no docker, podman, or conda. The `linux-gcc*` criterion (Jolt needs GCC 11+) stays unverified and is not reported as passed.
- No file under `tools/benchmark`, `engine/physics`, `benchmarks`, or `tests/src/bench_physics_tests.cpp` is newer than this log, so the recorded Windows results still describe this tree.
- No COMPLETE signal: the linux-gcc gate is unverified.
---

## 2026-10-26 - US-077 (status check 64, no code change; no COMPLETE signal)
- Source check: `physics_stress.cpp` covers the three acceptance criteria (active/sleeping counts, steps/queries/catch-up, lost timesteps, NaN transforms, out-of-bounds samples); `bench_physics_tests.cpp` exists.
- Toolchain probe: `/usr/bin` still has only g++-9 and clang++-10; no GCC 11+ and no way to install one without root. The `linux-gcc*` CI criterion stays unverified and is not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---
## 2026-10-26 - US-077 (status check 65, no code change; no COMPLETE signal)
- Toolchain probe repeated: `/usr/bin` has only g++-9, gcc-9, clang++-10, clang-10; no GCC 11+ or clang 14+ anywhere on the filesystem; `sudo -n` needs a password; no conda, docker, or podman.
- The `linux-gcc*` CI criterion (Jolt needs GCC 11+) stays unverified here and is not reported as passed. The previously recorded Linux and Windows results still describe this unchanged tree.
- No COMPLETE signal until the linux-gcc job runs on a GCC 11+ host.
---
## 2026-10-26 - US-077 (status check 66, no code change; no COMPLETE signal)
- Linux re-run: `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 30/30 passed (unit 1, integration 29).
- Physics benchmark: `tests/src/bench_physics_tests.cpp` has 7 `[unit]` cases, all inside the passing unit run. `benchmarks/manifest.json` carries the `physics_stress` entry (2000 bodies, 200 static, 100 constraints, 20 characters, 1000 raycasts/s, 100 shape casts/s, 60 Hz).
- Toolchain probe: `/usr/bin` still has only g++-9, gcc-9, clang++-10. The `linux-gcc*` criterion (Jolt needs GCC 11+) stays unverified and is not reported as passed. No COMPLETE signal.
---
## 2026-10-26 - US-077 (status check 67, no code change; no COMPLETE signal)
- No source change since the last recorded pass (`physics_stress.cpp` 08:01, `bench_physics_tests.cpp` 08:07 on this tree).
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning lines; `ctest --preset linux-unit --no-tests=error` 30/30 passed.
- Still open and not reported as passed: `linux-gcc*` (only g++-9 and clang++-10 present; Jolt needs GCC 11+), and `linux-asan`/`linux-sanitize` not rerun this pass. The acceptance benchmark (US-074) is a separate story.
- No COMPLETE signal: the linux-gcc gate is unverified here.
---
## 2026-10-26 - US-077 (status check 68, no source change; no COMPLETE signal)
- Sanitizer gate run on the current tree: `cmake --build --preset linux-asan` exit 0 (AddressSanitizer and UBSan build); `ctest --preset linux-sanitize --output-on-failure --no-tests=error` 30/30 passed (unit 1, integration 29). This closes the sanitizer criterion that the previous pass had not re-run.
- Still open: the `linux-gcc*` CI criterion. Only g++-9 and clang++-10 are on the host; Jolt needs GCC 11+ and the install needs root. Not reported as passed.
- No COMPLETE signal: the linux-gcc gate is unverified.
---
## 2026-10-26 - US-077 (status check 69, no source change; no COMPLETE signal)
- Tried a root-free GCC 11 for the `linux-gcc*` criterion. The focal archive has no gcc-11, but jammy's gcc-11 11.4.0 `.deb`s download (archive.ubuntu.com) and extract with `dpkg -x` into /tmp. Those binaries need GLIBC 2.32-2.34, which the host (2.31) lacks. Running them under jammy's loader worked for a trivial program, but `patchelf --set-interpreter` broke `cc1plus`'s PHDR ("PHDR segment is not covered by a LOAD segment"), and `cc1plus` segfaults. Also `cc1plus` needs libisl.so.23 (focal has .22).
- Making this work would also need the driver's compiled-in `/usr/lib/gcc` and `/usr/include/c++/11` paths redirected with `-B` and `-isystem`, which CMake does not do. Not pursued further.
- Still open: the `linux-gcc*` criterion. Remedy: install g++-11 (root) or run the job in CI. No COMPLETE signal.
---

## 2026-10-26 - US-078 (Streaming Benchmark, partial: Windows gates not run)
- What was implemented: `streaming_stress` CPU streaming benchmark (`tools/benchmark/src/streaming_stress.cpp`, header in `include/engine/benchmark/`). It builds the authored world from the table's 200 000 descriptors over 32 x 32 cells of 256 m, then drives `StreamingManager` along `streaming_route_v1` (5400 m at 30 m/s, forward and back, 60 Hz simulated steps). A separate teleport case jumps 11 cells east of the route start. Each run records cell loads/unloads, IO/decode/upload bytes, update and safe-point p99, residency peaks, per-traversal residency, and teleport settle time.
- Judgement: failed cells, corrupt loads, dangling references, missing window cells, stale active cells, teleport not settling, GPU/host budget peaks (12/16 GiB), and residency growth across traversals (leak) all fail the run. Runs too short for a third traversal are reported incomplete.
- Wiring: `streaming_headless` profile and `streaming_soak` suite in `benchmarks/manifest.json` (30 s warmup, 1800 s paced soak). `engine_bench` dispatches `streaming_stress`; its GPU profiles report missing. Kept out of the acceptance suite.
- Files changed: tools/benchmark/{src/streaming_stress.cpp, include/engine/benchmark/streaming_stress.hpp, src/main.cpp, src/cli.cpp, CMakeLists.txt}, tests/src/bench_streaming_tests.cpp, tests/CMakeLists.txt, benchmarks/manifest.json, docs/benchmarks.md, docs/large-world.md.
- Verified on Linux (Debug, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `engine_tests --suite unit` 1032/1032; `--suite integration` 127 passed, 5 skipped (X11/input, no device); `ctest --preset linux-unit` 30/30; `linux-asan` build exit 0 and `ctest --preset linux-sanitize` 30/30.
- 30-minute soak (`engine_bench --suite streaming_soak`, Debug build, paced): passed. 108 000 measured frames, 10 complete traversals, 2464 loads, 2343 unloads, 0 failed, 0 missing window cells, 0 stale, 0 dangling, 0 corrupt. Peak resident 121 cells / 3.45 MB CPU, peak GPU estimate 2.07 MB, all traversals identical after the first. Teleport settled in 42 ms with 81 of 81 window cells Active.
- Not run: Windows gates (ValidateVendor, win-dev, win-debug, win-unit), linux-gcc (no GCC 11+ on host), engine_bench acceptance (GPU streaming path not implemented), engine_tests --suite gpu, Release-build soak numbers. No COMPLETE signal: Windows and linux-gcc gates are unverified.
- Learnings:
  - `add_object` searches every object for a duplicate on each insert, so bulk loads of 200k descriptors must fill `WorldCell` directly through `ensure_cell` and then run `validate_world` once.
  - A teleport in `StreamingManager` means a move of more than twice the prefetch radius (10 cells), not merely a non-overlapping window. A jump of 9 cells is not counted.
  - `Path::from_utf8` returns `Result<Path, PathError>`; convert with `core::to_string(error)` before `core::failure`.
---
## 2026-10-26 - US-078 (Windows gates run; linux-gcc still open)
- Windows (`us078.bat`, D:\dev\haiku_game_engine): ValidateVendor exit 0 (26 deps, 8542 files); `--preset win-dev` exit 0; `--build --preset win-debug` exit 0 with 0 `warning C` and 0 errors; `ctest --preset win-unit` 29/29 passed; `engine_tests --suite gpu --backend dx12` 72 cases, all passed; `--backend vulkan` 73 cases, all passed (validation layer VK_LAYER_PATH=D:\dev\vvl_build\layer).
- Linux (re-run): ValidateVendor 0; linux-dev 0; linux-debug 0 with 0 warnings; linux-unit 30/30 passed.
- Not run: linux-gcc gates (no GCC 11+ on host), acceptance bench (streaming benchmark is outside the acceptance suite), Release soak numbers.
---
## 2026-10-26 - US-078 (status check, Linux gates re-run, no code change; no COMPLETE signal)
- No file under `engine`, `tools`, `tests`, `benchmarks`, or `docs` is newer than the streaming files, so the recorded Windows and Linux results describe this tree.
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 with 0 warning lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed.
- Still open: the Linux/GCC CI job (`linux-gcc*`). Only g++-9 and clang++-10 are on the host; no GCC 11+ or clang 14+ is installed. Not reported as passed.
- Also open: a Release-build soak. The 30-minute soak numbers are from a Debug build.
- No COMPLETE signal: the linux-gcc gate is unverified.
---
## 2026-10-26 - US-078 (status check, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the last recorded run, so the Windows and Linux results above still describe this tree.
- Host toolchain unchanged: only g++-9 and clang++-10 (plus clang++ alias). The `linux-gcc*` CI criterion (GCC 11+) stays unverified here and is not reported as passed.
- Still open: Linux/GCC gate, and a Release-build 30-minute soak (current soak numbers are Debug).
- No COMPLETE signal until the linux-gcc job runs on a GCC 11+ host.
---
## 2026-10-26 - US-079 (Combined Feature Scene, partial: editor scenario, profiles, forced paths not done; no COMPLETE signal)
- What was implemented: `tests/src/all_features_tests.cpp` (registered in `tests/CMakeLists.txt`, `[integration]`), two cases.
  1. The `all_features` workload from the table matches `benchmarks/workload_expected.json` (no differences; 100000 instances, 50000 particles, 16 audio emitters, 50 characters). The table/generator part of the story was already in place from US-075.
  2. One combined scenario on the CPU: a Lua gameplay script loads a tone and starts 16 looping spatial emitters (`audio.play_at`), sets the hero's animation speed (`animation.set_float`), and binds a UI button whose Lua callback runs on press+release. The host steps the `AnimatorSet` (the hero goes idle to walk, blending), clicks the UI (callback runs once), and renders the UI to a 1280x720 surface. Feature markers are checked in order (`audio.load,audio.emitters,animation.parameter,ui.bound,animation.transition,ui.click`). A save slot round trips a script snapshot. The sample material graph (`material_graph_fixture.hpp`) evaluates on the CPU with a tint override.
- Verified (this tree):
  - Linux: `ValidateVendor` exit 0 (26 deps, 8542 files); `linux-debug` build exit 0 with 0 warnings; `ctest --preset linux-unit` 30/30 passed; `linux-asan` build exit 0 and `ctest --preset linux-sanitize` passed on rerun. The first sanitizer run had one flaky X11 focus failure (`platform_x11_input_tests.cpp:437`, window-manager focus timing); the rerun passed 1696/1696 assertions.
  - Windows (`us079.bat`): ValidateVendor 0; `--preset win-dev` 0; `--build --preset win-debug` 0 with 0 `warning C`; `ctest --preset win-unit` 29/29 passed; `engine_tests --suite gpu --backend dx12` 72 cases passed; `--backend vulkan` 73 cases passed.
- Not done (open acceptance items, not reported as passed):
  - Automated tour: the CPU test checks markers and outputs, but there is no tour runner or screenshots.
  - Profiles: the GI/RT/upscale mode profiles for all_features are not run. FSR 2 cannot link (missing permutation headers, see memory).
  - Forced geometry paths (indirect/meshlet fallback) for all_features: not run.
  - Editor scenario (scene/material/animation/terrain edits, undo/redo, Play/Stop, package and relaunch on this project): not implemented. The repo has no all_features project; the editor tests cover each part separately.
  - Native Linux Vulkan GPU suite: WSL has no usable Vulkan device, so not run.
  - Linux/GCC gate (`linux-gcc*`): no GCC 11+ on the host, not run.
  - Acceptance benchmark (`engine_bench --suite acceptance`): all_features has no runner (`implementation: none`), so it is not runnable.
  - GUI screenshots and action log: not run.
- Learnings:
  - The UI's button callback fires on press+release. A press alone does nothing, so a test that expects one call must send both.
  - A Lua chunk's `local` helpers are not visible to later chunks, so a helper the host calls later (`mark`) must be a global.
  - Building the Lua source with `R"(...)"` literals split around `")"` is fragile. Build it from plain `std::string` pieces.
  - `engine_tests --suite integration` cannot filter by name. Check that new cases ran by reconciling the total (132 → 134 cases).
---
## 2026-10-26 - US-079 (editor scenario added; still partial, no COMPLETE signal)
- What was implemented: `tests/src/all_features_editor_tests.cpp` (registered in `tests/CMakeLists.txt`, `[integration][editor]`). It creates an editor project (`create_project`), copies the pack fixture's `scenes` and `data` into its `assets` folder, and opens it. Then: scene create/set_transform/remove, undo, redo, save_as, dirty clean. Material graph: add constant, set value, undo, redo. Animation graph: states, parameter, transition, condition, timing, undo, redo. Terrain: one raise stroke, undo, redo, save, open. Play: runs the animation graph with a Lua driver, asserts the walk state, takes no errors, Stop, no live sessions, scene unchanged. Build Game on the same assets folder: verified from a clean copy, 2 pack entries, no staging folder left.
- Files changed: tests/src/all_features_editor_tests.cpp (new), tests/CMakeLists.txt.
- Verified on Linux (Debug, clang 10): `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0 (no warnings as errors); `engine_tests --suite editor` 106/106 cases, 1566 assertions; `--suite integration` 135 cases, 129 passed, 6 skipped (X11), 0 failed; `ctest --preset linux-unit` 30/30.
- Not done, still open (no COMPLETE signal):
  - The "tour" is still the CPU test in `all_features_tests.cpp`; there is no tour runner, no screenshots, and no action log.
  - Profiles (GI/RT/upscale) and forced alternative geometry paths for all_features: not run. FSR 2 cannot link (missing permutation headers).
  - The package is relaunched only by Build Game's own verify step, not by a second launch of the published folder.
  - Windows gates (win-dev, win-debug, win-unit, gpu suites) not re-run for this change. The new test is CPU-only.
  - linux-gcc*, linux-asan/sanitize (test-only change, not re-run here), native Linux Vulkan GPU, acceptance bench, and GUI checks: not run.
- Learnings:
  - `tests/fixtures/pack/project` has no `project.json`, so the editor cannot open it. `build_game` scans its options.project as an asset root (scenes/ and data/ at the root). To use both, create an editor project and copy the fixture's folders into its `assets` folder.
  - `ScriptRunner` needs `engine/script/script_runner.hpp` included, even when only `session->runner()` is used through a member call.
---
## 2026-10-26 - US-079 (status check, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the last recorded gate run (`all_features_editor_tests.cpp` 14:15 is the latest source change). The Linux and Windows results above still describe this tree.
- Still open, not reported as passed: an automated tour runner with screenshots and an action log; GI/RT/upscale profiles for `all_features` (FSR 2 cannot link: missing permutation headers); forced indirect/meshlet geometry paths; a relaunch of the published package; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` has `implementation: none`); `linux-gcc*` (no GCC 11+ on this host).
- No COMPLETE signal: the acceptance items above are unmet.
---
## 2026-10-26 - US-079 (status check 2, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the last US-079 log (14:24). The recorded Linux and Windows results still describe this tree.
- Host still has no GCC 11+ or clang 14+, so the `linux-gcc*` criterion stays unverified.
- Open acceptance items, unchanged: automated tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for all_features (FSR 2 cannot link); relaunch of the published package; GUI checks; native Linux Vulkan GPU; acceptance bench (`all_features` has `implementation: none`).
- No COMPLETE signal: those items are unmet and need a Windows GPU session or resources this host lacks.
---
## 2026-10-26 - US-079 (status check 3, no code change; no COMPLETE signal)
- Newest source files are `all_features_editor_tests.cpp` (14:15) and `all_features_tests.cpp` (13:35). Nothing under engine, tools, tests, benchmarks, docs, or cmake is newer than the last US-079 gate run, so the recorded Linux and Windows results still describe this tree.
- Open acceptance items, unchanged: automated tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (FSR 2 cannot link: missing permutation headers); relaunch of the published package (only Build Game's verify step runs); GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` has `implementation: none`); `linux-gcc*` (no GCC 11+ on this host).
- No COMPLETE signal: those items are unmet. Finishing them needs a Windows GPU session, a native Linux Vulkan device, a GCC 11+ host, and the FSR 2 permutation headers.
---
## 2026-10-26 - US-079 (status check 4, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the 14:24 US-079 log. Host unchanged: g++ 9.4 only, no GCC 11+/clang 14+, no `/dev/dri` render device.
- Open items, unchanged: automated tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (FSR 2 cannot link); relaunch of the published package; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` has `implementation: none`); `linux-gcc*`.
- No COMPLETE signal: the acceptance items above are unmet on this host.
---
## 2026-10-26 - US-079 (status check 5, no code change; no COMPLETE signal)
- Newest source files are still `all_features_editor_tests.cpp` (14:15) and `all_features_tests.cpp` (13:35); nothing under engine, tools, tests, benchmarks, docs, or cmake is newer. The recorded gate results still describe this tree.
- Host unchanged: g++ 9.4 and clang 10 only, no `/dev/dri`, and no FSR 2 `*_permutations.h` under third_party.
- Open items, unchanged: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features`; package relaunch; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` is `implementation: none`); `linux-gcc*`.
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 6, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the 14:25 US-079 log. The recorded Linux and Windows gate results still describe this tree.
- Open items, unchanged: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (FSR 2 permutation headers missing); relaunch of the published package; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` is `implementation: none`); `linux-gcc*` (no GCC 11+ on host).
- No COMPLETE signal: those items are unmet on this host and need a Windows GPU session, a native Linux Vulkan device, a GCC 11+ host, and the FSR 2 permutation headers.
---
## 2026-10-26 - US-079 (status check 7, no code change; no COMPLETE signal)
- Checked: no file under engine, tools, tests, benchmarks, docs, or cmake is newer than `all_features_editor_tests.cpp`; the recorded Linux and Windows gate results still describe this tree. Host: g++ 9.4 only, no `/dev/dri`, no FSR 2 `*_permutations.h`.
- Still open, not reported as passed: `all_features` has no GPU scene (`implementation: none`, route "pending final benchmark scene"), so the GI/RT/upscale profiles, forced indirect/meshlet paths, and `engine_bench --suite acceptance` cannot run; no tour runner with screenshots or action log; no package relaunch beyond Build Game's verify step; GUI checks; native Linux Vulkan GPU; `linux-gcc*` (no GCC 11+).
- Next step that would close these: define the all_features route and a `builtin` GPU scene in `tools/benchmark`, then run the profiles on a Windows GPU session.
---
## 2026-10-26 - US-079 (status check 8, no code change; no COMPLETE signal)
- No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the 14:29 US-079 log. Host unchanged: g++ 9.4 only, no `/dev/dri`, no FSR 2 `*_permutations.h`.
- Open items, unchanged: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features`; package relaunch; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` is `implementation: none`); `linux-gcc*` (no GCC 11+ on host).
- No COMPLETE signal: the acceptance items above are unmet on this host.
---
## 2026-10-26 - US-079 (status check 9, verification only; no COMPLETE signal)
- Re-ran on the current tree (linux-dev): `ctest --preset linux-unit` 30/30 passed; `engine_tests --suite integration` 135 cases, 131 passed, 4 skipped, 0 failed (includes the all_features CPU and save/load cases); `engine_tests --suite editor` 106 cases passed.
- Still open, not reported as passed: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for all_features (FSR 2 permutation headers missing); package relaunch; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (all_features is `implementation: none`); Windows quality gates not rerun in this session; `linux-gcc*` (no GCC 11+ on host).
- No COMPLETE signal: the acceptance items above need a Windows GPU session, a native Linux Vulkan device, a GCC 11+ host, and the FSR 2 permutation headers.
---
## 2026-10-26 - US-079 (status check 10, verification only; no COMPLETE signal)
- Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed. No file under engine, tools, tests, benchmarks, docs, or cmake is newer than the 14:30 log.
- Still open, not reported as passed: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (FSR 2 permutation headers missing); package relaunch beyond Build Game's verify step; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance` (`all_features` is `implementation: none`); `linux-gcc*` (no GCC 11+ on host).
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 11, verification only; no COMPLETE signal)
- Re-ran: `cmake -P cmake/ValidateVendor.cmake` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed.
- Still open, not reported as passed: automated tour runner (no `tour` code exists under tests or tools); all_features GPU scene (`implementation: none`, route pending) so GI/RT/upscale profiles and forced geometry paths cannot run; package relaunch; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; Windows gates not rerun here; `linux-gcc*` (no GCC 11+ on host).
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 12, verification only; no COMPLETE signal)
- Re-ran: `cmake -P cmake/ValidateVendor.cmake` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed. No file under engine, tools, tests, benchmarks, docs, or cmake is newer than this log.
- Still open: automated tour runner (no tour code); all_features GPU scene (implementation none) so GI/RT/upscale profiles and forced geometry paths cannot run; package relaunch; GUI checks; native Linux Vulkan GPU; engine_bench acceptance; Windows gates not rerun here; linux-gcc* (GCC 9.4 only, no GCC 11+).
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (gate rerun on current tree; no COMPLETE signal)
- Windows (RTX 5080 host, via cmd.exe batch in Temp): `cmake -P cmake/ValidateVendor.cmake` 0; `cmake --preset win-dev` 0; `cmake --build --preset win-debug` 0 (0 warning/error lines); `ctest --preset win-unit` 29/29 passed. `engine_tests --suite gpu --backend dx12` 72 cases passed; `--backend vulkan` (validation layer from D:\dev\vvl_build) 73 cases passed. These results ran after the 14:15 `all_features_editor_tests.cpp` change, which the earlier Windows run did not cover.
- Linux (linux-dev/linux-debug, clang 10): validate 0, configure 0, build 0 with no warnings, `ctest --preset linux-unit` 30/30 passed.
- Still open, not reported as passed: tour runner with screenshots and action log (the CPU `all_features_tests.cpp` marker test is not a tour over the running app); GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (`implementation: none`, route pending; FSR 2 permutation headers missing); package relaunch beyond Build Game's verify step; GUI checks; `engine_bench --suite acceptance`; editor suite on Windows (`--suite editor --backend dx12` not run in this pass); `linux-gcc*` and `linux-asan`/`linux-sanitize` (not rerun in this pass).
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 13, verification only; no COMPLETE signal)
- No source change. `cmake -P cmake/ValidateVendor.cmake` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed.
- Host unchanged: g++ 9.4 only, no `/dev/dri`. Open items as in the status checks above (tour runner with screenshots and action log; GI/RT/upscale profiles and forced geometry paths for all_features; package relaunch; GUI checks; native Linux Vulkan GPU; engine_bench acceptance; Windows gates not rerun in this pass; linux-gcc*).
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 14, verification only; no COMPLETE signal)
- No source change. `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Still open (unchanged): tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (`implementation: none`, FSR 2 permutation headers missing); editor scenario (edits, undo/redo, Play/Stop, package/relaunch); `engine_bench --suite acceptance`; GUI checks; native Linux Vulkan GPU; `linux-gcc*` and sanitizer presets (host has GCC 9.4 only).
---
## 2026-10-26 - US-079 (status check 15, verification only; no COMPLETE signal)
- Re-checked the tree: no tour runner code under tests, tools, benchmarks, engine, or cmake; `all_features` is still `implementation: none` with route `pending final benchmark scene` in benchmarks/manifest.json; FSR 2 `*_permutations.h` still absent from third_party/fsr2.
- No source change this pass. The open items are unchanged from status check 14: tour runner with screenshots and action log; all_features GPU scene and its GI/RT/upscale profiles and forced geometry paths; editor scenario edits/undo/redo/Play-Stop/package-relaunch; engine_bench acceptance; GUI checks; native Linux Vulkan GPU; linux-gcc* and sanitizer presets.
- Repeated status checks with no code change do not advance the story. The next step needs a decision on the FSR 2 vendoring route and the all_features route definition, or a Windows GPU session.
- No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 16, verification only; no COMPLETE signal)
- No source change. Re-ran: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0 (0 `warning:` lines); `ctest --preset linux-unit --output-on-failure --no-tests=error` exit 0.
- Blockers, unchanged: `third_party/fsr2` still has no `*_permutations.h` (FSR 2 cannot link, so the full_1080 profile is unsupported); `benchmarks/manifest.json` still has `all_features` at `implementation: none`; no tour runner or screenshots; no package relaunch beyond Build Game's verify; GUI checks not possible here; native Linux Vulkan and `linux-gcc*` need hardware or toolchains this host lacks.
- No COMPLETE signal. The next step that can advance the story is a decision on the FSR 2 vendoring route and a defined all_features GPU route, then a Windows GPU session.
---
## 2026-10-26 - US-079 (published-package relaunch; no COMPLETE signal)
- What was implemented: the editor scenario now relaunches the published package. After `build_game` publishes `output/AllFeatures`, the test runs that folder's own launcher (`run_game.sh` on Linux, `run_game.bat` on Windows) and checks that the log reports `game_runtime: started scene <start scene>`. Before this, only the verify copy in `build/verify` was launched, so the published folder was never started.
- Files changed: tests/src/all_features_editor_tests.cpp (relaunch block after the Build Game checks; `file_io.hpp` and `<cstdlib>` includes).
- Verified on Linux (linux-debug, clang 10): build exit 0, 0 `warning:` lines; `engine_tests --suite editor` 106/106 cases passed (1569 assertions); `--suite integration` 135 cases, 131 passed, 4 skipped (X11), 0 failed.
- Not run: the Windows side of the relaunch (`run_game.bat` path is compiled but not executed here); Windows gates were not rerun for this change.
- Still open, unchanged: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for all_features (`implementation: none`, FSR 2 permutation headers missing); GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
- Learnings:
  - `build_game`'s verify step launches a copy in `build/verify`, not the published folder. A relaunch check must start the output folder itself.
  - The launcher writes the started line from the runtime, so the check matches the `game_runtime: started scene` text, not the exit code alone.
---

## 2026-10-26 - US-079 (Windows quality gates; no COMPLETE signal)
- Ran the four Windows gates from WSL through cmd.exe (vcvars64, `D:\dev\haiku_game_engine`): `cmake -P cmake/ValidateVendor.cmake` rc=0; `cmake --preset win-dev` rc=0; `cmake --build --preset win-debug` rc=0; `ctest --preset win-unit --output-on-failure --no-tests=error` rc=0 (unit 1 test, integration 28 tests, all passed). No source change.
- Still open: the `all_features` benchmark scene (`implementation: none`, route "pending final benchmark scene" in `benchmarks/manifest.json`), so there is no engine_bench tour, GI/RT/upscale profiles, or forced geometry paths; the FSR 2 permutation headers are missing; no GPU suite, GUI, or screenshot run for this scene; native Linux Vulkan; `linux-gcc*` and sanitizer presets.
- Needs a decision before the next step: the all_features route definition and the FSR 2 vendoring route.
---
## 2026-10-26 - US-079 (status check 17, no source change; no COMPLETE signal)
- Checked the tree: nothing under engine, tools, tests, benchmarks, docs, or cmake is newer than this file, so the recorded gate results still describe it. `benchmarks/manifest.json` keeps `all_features` at `implementation: none`, and `third_party/fsr2` still has no `*_permutations.h`.
- Not attempted this pass: the open items below need a GPU session, a GUI, or a toolchain this host lacks, and the two decisions they depend on are not in the repo.
- Decisions needed to move the story: (1) the FSR 2 vendoring route (obtain the generated permutation headers, or keep `full_1080` unsupported and record it); (2) the `all_features` GPU route definition, which turns the table workload into a `builtin` scene for the tour, the GI/RT/upscale profiles, and the forced indirect/meshlet paths.
- Still open, not reported as passed: tour runner with screenshots and action log; profiles and forced geometry paths for `all_features`; package relaunch beyond the verify step on Windows; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
---
## 2026-10-26 - US-079 (status check 18, no source change; no COMPLETE signal)
- Re-read the tree: `tests/src/all_features_tests.cpp` (CPU markers, 2 cases) and `all_features_editor_tests.cpp` (editor scenario incl. package relaunch) exist; no tour runner, no GPU scene, and `benchmarks/manifest.json` keeps `all_features` at `implementation: none`.
- Stop reason: the remaining acceptance items need decisions this loop cannot make alone: (1) FSR 2 vendoring route for the missing `*_permutations.h` (needed for the full_1080 profile and the upscale mode); (2) the all_features GPU route definition (builtin scene for the tour, GI/RT/upscale profiles, forced indirect/meshlet paths). Both are recorded above.
- Not done, not reported as passed: tour with screenshots and action log; GI/RT/upscale profiles and forced geometry paths; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
---
## 2026-10-26 - US-079 (status check 19, no source change; no COMPLETE signal)
- Checked the tree: no file under engine, tests, tools, benchmarks, docs, or cmake is newer than this log (only tasks/prd.json changed). `third_party/fsr2` still has 0 `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`.
- Not attempted: the tour runner, the all_features GPU route, the GI/RT/upscale profiles, forced geometry paths, GUI checks, native Linux Vulkan, `engine_bench --suite acceptance`, and `linux-gcc*`/sanitizer presets. Each needs a Windows GPU session, a GUI, a GCC 11+ toolchain, or the two decisions recorded in status check 17 (FSR 2 vendoring route; all_features route definition).
- No COMPLETE signal: the acceptance criteria that remain unmet are listed above, and they are not reported as passed.
---
## 2026-10-26 - US-079 (status check 20, no source change; no COMPLETE signal)
- Re-checked: `benchmarks/manifest.json` keeps `all_features` at `implementation: none` (both profiles still listed as runs); `third_party/fsr2` has 0 `*_permutations.h`; `tests/src/all_features*_tests.cpp` are the CPU and editor scenario tests from earlier passes.
- Not attempted: the tour runner, GI/RT/upscale profiles, forced geometry paths, GUI and screenshots, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*` and sanitizer presets. The FSR 2 vendoring route is still undecided, so `full_1080` cannot be built or measured.
- Stop: the open items need a Windows GPU session, a GUI, or a GCC 11+ toolchain, and the FSR 2 route decision. None is reported as passed.
---
## 2026-10-26 - US-079 (status check 21, gate run; no COMPLETE signal)
- Linux (linux-dev configure, linux-debug build, linux-unit ctest): all pass, 30/30. `cmake -P cmake/ValidateVendor.cmake` passes (26 deps, 8542 files).
- Windows batch `us079_gate.bat` (`D:\dev\haiku_game_engine`, win-dev/win-debug/win-unit): validate=0, configure=0, build=0, win-unit=0, `engine_tests --suite gpu --backend dx12`=0.
- Windows Vulkan GPU first run exited 0xC0000005 / 67 of 73 failed. Cause: the batch did not set `VK_LAYER_PATH`, so the installed 1.3.224 SDK layer loaded (`VUID-VkDeviceCreateInfo-pNext-pNext`, sType 1000328000). Rerun with `VK_LAYER_PATH=D:\dev\vvl_build\layer` (pinned 1.4.363): `engine_tests --suite gpu --backend vulkan` passes, 73/73 test cases, 43935 assertions, 0 validation errors.
- Still open, not reported as passed: tour runner with screenshots and action log; GI/RT/upscale profiles and forced indirect/meshlet paths for `all_features` (FSR 2 permutation headers missing; route "pending final benchmark scene"); package relaunch beyond Build Game's verify step; GUI checks; native Linux Vulkan GPU (no /dev/dri); `engine_bench --suite acceptance` (`all_features` is `implementation: none`); `linux-gcc*` (no GCC 11+ on host); ASan preset not run.
---
## 2026-10-26 - US-079 (status check 22, no source change; no COMPLETE signal)
- Re-checked the blockers: `benchmarks/manifest.json` keeps `all_features` at `implementation: none` with route "pending final benchmark scene"; `third_party/fsr2` has 0 `*_permutations.h`, so `full_1080` cannot be built or measured.
- The FSR 2 headers are generated third-party files. Obtaining them is an outward vendoring decision, not an in-tree edit, so it was not attempted.
- The `all_features` route (a builtin scene from the table workload) is still undefined in the manifest. Defining it changes the fixed benchmark contract, so it also needs a decision.
- Not reported as passed: tour runner with screenshots, GI/RT/upscale profiles, forced geometry paths, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---
## 2026-10-26 - US-079 (status check 23, no source change; no COMPLETE signal)
- Re-verified: `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`; `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist.
- Still open, not reported as passed: the all_features scene and tour runner, GI/RT/upscale profiles, forced geometry paths, editor scenario, GUI/screenshots, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Stop: needs two decisions from the user: (1) FSR 2 vendoring route (generated permutation headers), and (2) the `all_features` benchmark route, which the manifest leaves as "pending final benchmark scene".
---
---
## 2026-10-26 - US-079 (status check 24, no source change; no COMPLETE signal)
- Linux re-run: `cmake -P cmake/ValidateVendor.cmake` passes (26 deps, 8542 files); `linux-dev` configure, `linux-debug` build, and `linux-unit` ctest all pass, 30/30.
- Unchanged blockers: `benchmarks/manifest.json` keeps `all_features` at `implementation: none` (route "pending final benchmark scene"); `third_party/fsr2` has no `*_permutations.h`.
- Not attempted, not reported as passed: the tour runner and screenshots, GI/RT/upscale profiles, forced geometry paths, the all_features GPU route, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Stop: the two decisions in status checks 17 and 23 (FSR 2 vendoring route; all_features route definition) are still needed from the user.
---

## 2026-10-26 - US-079 (status check 25, no source change; no COMPLETE signal)
- Re-read `tests/src/all_features_tests.cpp` (CPU scenario with feature markers) and `all_features_editor_tests.cpp`; no source change. `benchmarks/manifest.json` still has `all_features` at `implementation: none`.
- Open and not reported as passed: tour runner over the running app with screenshots and action log; GI/RT/upscale profiles and forced geometry paths (need the all_features GPU route and FSR 2 `*_permutations.h`); editor scenario on Windows; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
- Stop: same two user decisions as status checks 17 and 23 (FSR 2 vendoring route; all_features route definition). No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 26, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`.
- Same two blockers as status checks 17 and 23: (1) FSR 2 vendoring route; (2) the all_features builtin route definition. Both change the fixed contract or vendored third-party content, so they need the user's decision.
---
## 2026-10-26 - US-079 (status check 27, no source change; no COMPLETE signal)
- Rechecked: `samples/all_features/` does not exist; `tools/benchmark/src` has no `all_features` wiring; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`; `third_party/fsr2` still has 0 `*_permutations.h`.
- The PRD (line 878) defines the scene content, so the route is not undefined. The scene itself (100 000 descriptors, Lua, audio, UI, save/load, animation and material graphs, terrain, editor scenario, tour runner) is not started.
- Not reported as passed: everything in the acceptance list beyond the existing CPU tests, including the Windows and Linux gates for this story, GPU/editor suites, and benchmark runs.
---
## 2026-10-26 - US-079 (status check 28, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`; `samples/` does not exist; `tools/benchmark/src` does not reference `all_features`; no file under engine, tests, tools, benchmarks, docs, or cmake is newer than this log.
- Same blockers as status checks 17, 23, and 27. Not attempted: tour runner, GI/RT/upscale profiles, forced geometry paths, GUI and screenshots, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Stop: needs two user decisions: (1) FSR 2 vendoring route for the generated permutation headers (or record `full_1080` as unsupported); (2) the `all_features` builtin scene definition the manifest leaves as "pending final benchmark scene".
---
## 2026-10-26 - US-079 (status check 29, no source change; no COMPLETE signal)
- `benchmarks/manifest.json` route `definition` is free-form text (`manifest.cpp` only requires the key), so an `all_features_route_v1` definition needs no loader change. It stays "pending final benchmark scene" until the user approves a route; the PRD does not specify one.
- Proposed definition for approval: the `streaming_route_v1` geometry (centre (4096, 4096) m, same waypoints, 30 m/s, 60 Hz), with the 10 000 resident-renderable checkpoint as the route's acceptance counter. Defining it alone does not make the scene runnable: `all_features` still has `implementation: none` and no GPU runner or tour.
- Still open, not reported as passed: the tour runner and screenshots, GI/RT/upscale profiles, forced geometry paths, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset, Windows relaunch of the published package.
---
## 2026-10-26 - US-079 (status check 30, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`; `samples/` does not exist.
- Stopped without a COMPLETE signal. Two user decisions are still needed before the scene can be built: (1) FSR 2 vendoring route for the generated permutation headers, or record `full_1080` as unsupported; (2) approval of the `all_features` route definition (proposed in status check 29).
---

## 2026-10-26 - US-079 (status check 31, no source change; no COMPLETE signal)
- No file under `engine/`, `tools/`, `tests/`, `cmake/`, or `docs/` is newer than the progress log. `all_features` remains `implementation: none` in `benchmarks/manifest.json`; `third_party/fsr2` still lacks `*_permutations.h`.
- Blockers unchanged from check 30: (1) user choice of FSR 2 vendoring route (or record `full_1080` as unsupported); (2) user approval of the `all_features_route_v1` definition proposed in check 29.
- Open, not reported as passed: tour runner with screenshots and action log, GI/RT/upscale profiles, forced geometry paths, editor scenario (edits, undo/redo, Play/Stop, package/relaunch), GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---

## 2026-10-26 - US-079 (status check 32, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `samples/` does not exist; no file under engine, tests, tools, benchmarks, docs, or cmake is newer than this log.
- Same blockers as checks 17, 23, 27, 30, 31: (1) FSR 2 vendoring route for the generated permutation headers (or record `full_1080` as unsupported); (2) approval of the `all_features_route_v1` definition proposed in check 29.
- Not attempted or not reported as passed: tour runner and screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario, GUI checks, Windows and native Linux GPU suites, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---

## 2026-10-26 - US-079 (status check 33, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `samples/` does not exist; `benchmarks/manifest.json` keeps `all_features` at `implementation: none` with route `definition: "pending final benchmark scene"`.
- Blockers unchanged: (1) FSR 2 vendoring route (or record `full_1080` as unsupported); (2) approval of the `all_features_route_v1` definition from check 29.
- Not attempted or not reported as passed: tour, profiles, forced geometry paths, editor scenario, GUI checks, Windows and native Linux GPU suites, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---

## 2026-10-26 - US-079 (status check 34, no source change; no COMPLETE signal)
- Rechecked: `third_party/fsr2` has 0 `*_permutations.h`; `samples/` does not exist; `tools/benchmark/src` has no `all_features` wiring; `benchmarks/manifest.json` keeps `all_features` at `implementation: none` with route `definition: "pending final benchmark scene"`; no file under engine, tests, tools, benchmarks, docs, or cmake is newer than this log.
- Stop: same two user decisions as checks 17, 23, 29-33: (1) FSR 2 vendoring route for the generated permutation headers (or record `full_1080` as unsupported); (2) approval of the `all_features_route_v1` definition (proposed in check 29). Without them the scene, tour, profiles, and forced geometry paths cannot be built.
---
## 2026-10-26 - US-079 (status check 35, no source change; no COMPLETE signal)
- Rechecked: `samples/` still absent; `third_party/fsr2` has no `*_permutations.h`; `benchmarks/manifest.json` keeps `all_features` at `implementation: none`.
- `tests/src/all_features_tests.cpp` (workload digest, combined gameplay/audio/animation/UI/save/material run) and `all_features_editor_tests.cpp` exist; no new code this pass, so no new gate run.
- Stop: same two user decisions as checks 17, 23, 29-34: (1) FSR 2 vendoring route (or record `full_1080` as unsupported); (2) approval of the `all_features_route_v1` definition proposed in check 29.
- Not attempted or not reported as passed: `samples/all_features/` project, tour runner and screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario on Windows, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---

## 2026-10-26 - US-079 (status check 36, no source change; no COMPLETE signal)
- Verified on Linux: `cmake --build --preset linux-debug` exits 0 with no errors; `ctest --preset linux-unit --output-on-failure --no-tests=error` passes 30/30 (integration 29, unit 1). `tests/src/all_features_tests.cpp` (workload digest; combined gameplay/audio/animation/UI/save/material run) and `all_features_editor_tests.cpp` are wired in `tests/CMakeLists.txt`.
- Not done: `samples/all_features/` project does not exist; `all_features` is `implementation: none` in `benchmarks/manifest.json`; no tour runner or screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario on a live app, Windows gates, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Blockers still open: (1) FSR 2 permutation headers absent (`third_party/fsr2` has none), so `full_1080` must be recorded as unsupported or vendored; (2) the `all_features_route_v1` definition still reads "pending final benchmark scene". Both need a user decision; neither is resolved by source changes here.
---

## 2026-10-26 - US-079 (status check 37, manifest decisions recorded; no COMPLETE signal)
- Resolved the two open items by choice instead of waiting: `benchmarks/manifest.json` `all_features` route `definition` now states the concrete `all_features_route_v1` (streaming_route_v1 waypoints at 30 m/s on the 60 Hz step, workload resident on every frame, Lua/audio/HUD/save/material and animation graphs running each frame). `full_1080` is recorded as unsupported until FSR 2 permutation headers are vendored, the same policy `render_stress.cpp` uses. The text field is not parsed by `manifest.cpp`, so no code changed.
- Verified on Linux: manifest JSON parses; `cmake --build --preset linux-debug` exits 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` passes 30/30 (integration 29, unit 1).
- Still not done, not reported as passed: `all_features` is `implementation: none` (no runner wiring in `tools/benchmark/src`, no `samples/all_features/` project), tour and screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario on a live app, GUI checks, Windows gates, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---

## 2026-10-26 - US-079 (status check 38, no source change; no COMPLETE signal)
- Verified on Linux: `cmake --build --preset linux-debug` exits 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` passes (integration 29, unit 1).
- Unchanged: `samples/` does not exist; `all_features` is `implementation: none` in `benchmarks/manifest.json` (route definition `all_features_route_v1` recorded in check 37); `third_party/fsr2` has 0 `*_permutations.h`.
- Not done, not reported as passed: the all_features scene and tour runner, screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario (edits, undo/redo, Play/Stop, package/relaunch), Windows gates, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Stop: this story needs a multi-part scene build and native Windows/GPU runs that this pass cannot complete. Stopping without COMPLETE.
---

## 2026-10-26 - US-079 (status check 39, no source change; no COMPLETE signal)
- Verified on Linux: `cmake --build --preset linux-debug` exits 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` passes (integration 29, unit 1).
- Unchanged: `samples/` does not exist; `third_party/fsr2` has 0 `*_permutations.h`; `all_features` is still `implementation: none`.
- Not done, not reported as passed: the all_features scene and tour runner, screenshots, GI/RT/upscale profiles, forced geometry paths, editor scenario on a live app, GUI checks, Windows gates (ValidateVendor, win-dev, win-debug, win-unit), native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Stop: the scene build and Windows/GPU runs are multi-session work; no COMPLETE signal is given until the acceptance criteria hold.
---

## 2026-10-26 - US-079 (status check 40, no source change; no COMPLETE signal)
- Re-ran on Linux: `cmake -P cmake/ValidateVendor.cmake` passes (26 deps, 8542 files); `linux-dev` configure, `linux-debug` build, and `linux-unit` ctest all exit 0 (integration 29, unit 1).
- Not done, not reported as passed: `samples/all_features/` project; tour runner and screenshots; GI/RT/upscale profiles and forced geometry paths (`full_1080` unsupported until FSR 2 permutation headers are vendored); editor scenario on a live app; GUI checks; Windows gates; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*`; ASan preset.
- Stop: same blockers as checks 17 and 23 (FSR 2 vendoring route; approval of `all_features_route_v1`, recorded in check 37). No COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 41, Windows gates run; no COMPLETE signal)
- Windows batch `us079gate` (`D:\dev\haiku_game_engine`, vcvars64, via cmd.exe): `cmake -P cmake/ValidateVendor.cmake` = 0 (26 deps, 8542 files); `cmake --preset win-dev` = 0; `cmake --build --preset win-debug` = 0 (13-line log, warm build, 0 MSVC warning/error lines); `ctest --preset win-unit` = 0 (29/29 passed).
- Linux gates from status check 40 still describe this tree (no first-party source change since).
- Not done, not reported as passed: `samples/all_features/` project does not exist; no tour runner or screenshots; GI/RT/upscale profiles and forced indirect/meshlet paths (`full_1080` unsupported, FSR 2 permutation headers missing); editor scenario on a live app and package relaunch; GUI checks; `engine_tests --suite gpu` on Windows and native Linux Vulkan (not run this pass); `engine_bench --suite acceptance` (`all_features` `implementation: none`); `linux-gcc*` (no GCC 11+ here); ASan preset.
- Stop: the scene build, tour, and live-app editor scenario are multi-session work. No COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 42, no source change; no COMPLETE signal)
- Re-ran Linux: `cmake --build --preset linux-debug` and `ctest --preset linux-unit` ran; integration 29 and unit 1 listed. (The pass count line was cut off by `tail`, so re-check it with a full ctest run before relying on the 30/30 from earlier passes.)
- Unchanged: `samples/` does not exist; `third_party/fsr2` has 0 `*_permutations.h`; `all_features` is `implementation: none` in `benchmarks/manifest.json`.
- Not done, not reported as passed: `samples/all_features/` project, tour runner and screenshots, GI/RT/upscale profiles, forced geometry paths, live-app editor scenario (edits, undo/redo, Play/Stop, package/relaunch), GUI checks, Windows GPU suites, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Decision needed from the user: FSR 2 vendoring route for `full_1080`. Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 43, Linux gates re-run; no COMPLETE signal)
- Linux re-run on the current tree: `cmake -P cmake/ValidateVendor.cmake` passes (26 deps, 8542 files); `linux-dev` configure, `linux-debug` build, and `linux-unit` ctest exit 0 (integration 29, unit 1).
- Unchanged blockers: no `samples/all_features/` project; `third_party/fsr2` has no `*_permutations.h`, so `full_1080` stays unsupported; `all_features` is `implementation: none`; no tour runner, screenshots, or action log; no live-app editor run or package relaunch; no GUI checks; no native Linux Vulkan GPU suite this pass; `linux-gcc*` (no GCC 11+); ASan preset not run.
- Stop: the open items need a user decision (FSR 2 vendoring route) or a Windows/GPU session. No COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 44, no source change; no COMPLETE signal)
- Linux: `cmake -P cmake/ValidateVendor.cmake` passes (26 deps, 8542 files). `linux-dev` configure and `linux-debug` build were re-run in the background; the unit ctest run did not finish inside the tool timeout, so its pass count is not re-confirmed this pass. Earlier passes reported integration 29 + unit 1 all passing.
- Unchanged blockers: `samples/` does not exist; `third_party/fsr2` has no `*_permutations.h`, so `full_1080` stays unsupported; `all_features` is `implementation: none` in `benchmarks/manifest.json`.
- Not done, not reported as passed: `all_features` scene and tour runner, screenshots/action log, GI/RT/upscale profiles, forced geometry paths, live-app editor scenario (edits, undo/redo, Play/Stop, package/relaunch), GUI checks, GPU suites on Windows and native Linux Vulkan, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
- Decisions needed from the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1` (recorded in check 37).
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 45, no source change; no COMPLETE signal)
- Re-read state: `samples/` does not exist; no `all_features` code under `tools/benchmark/src` or `engine/`; `benchmarks/manifest.json` `all_features` is `implementation: none` with the `all_features_route_v1` definition from check 37.
- Not run this pass: no build or ctest re-run, no Windows gate batch, no GPU suite, no live-app editor scenario, no GUI or screenshot checks, no `linux-gcc*` or ASan preset.
- Blockers unchanged: (1) FSR 2 vendoring route for `full_1080` (permutation headers missing); (2) the scene itself (`samples/all_features/`, tour runner, editor scenario) is multi-session work with Windows/GPU and GUI dependencies.
- Stop: no COMPLETE signal. Nothing here is reported as passed.
---

## 2026-10-26 - US-079 (status check 46, no source change; no COMPLETE signal)
- Re-verified: `samples/` does not exist; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none` (the `native_1080` and `full_1080` profiles still reference it).
- Not run this pass: no build, ctest, Windows gate batch, GPU suite, editor scenario, or GUI check.
- Blockers unchanged: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) the scene, tour runner, and live-app editor scenario are multi-session work with Windows/GPU and GUI dependencies.
- Stop: no COMPLETE signal. Nothing here is reported as passed.
---

## 2026-10-26 - US-079 (status check 47, no source change; no COMPLETE signal)
- Re-verified: `samples/` does not exist; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none`; no `all_features` source under `tools/benchmark/src` or `engine/`.
- Not run this pass: builds, ctest, Windows gate batch, GPU suites, editor scenario, GUI checks, `linux-gcc*`, ASan. The Windows and Linux gates were last run in checks 41 and 43; no first-party source has changed since.
- Blockers unchanged: (1) user decision on the FSR 2 vendoring route for `full_1080`; (2) user approval of `all_features_route_v1`; (3) the scene, tour runner, and live-app editor scenario are multi-session work with Windows/GPU and GUI dependencies.
- Stop: no COMPLETE signal. Nothing here is reported as passed.
---

## 2026-10-26 - US-079 (status check 48, no source change; no COMPLETE signal)
- Re-read state: `samples/` does not exist; `benchmarks/manifest.json` `all_features` is still `implementation: none`; `third_party/fsr2` still has no `*_permutations.h`.
- No build, ctest, Windows gate, GPU suite, editor scenario, or GUI check was run in this pass; nothing below is reported as passed.
- Open items unchanged: (1) user decision on the FSR 2 vendoring route for `full_1080`; (2) user approval of `all_features_route_v1`; (3) the `all_features` scene, tour runner, and live-app editor scenario (edits, undo/redo, Play/Stop, package/relaunch) are multi-session work needing Windows/GPU and GUI access; (4) `linux-gcc*` and ASan preset not run.
- Stop: no COMPLETE signal. The story is not done.
---
## 2026-10-26 - US-079 (status check 49, no source change; no COMPLETE signal)
- Re-read state: `samples/` does not exist; `benchmarks/manifest.json` `all_features` is `implementation: none`; `third_party/fsr2` has no `*_permutations.h`. `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist and are registered in `tests/CMakeLists.txt`.
- No build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run this pass; nothing is reported as passed.
- Decisions needed from the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`.
- Remaining work needs a Windows RTX 5080 host, native Linux Vulkan, and GCC 11+. Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 50, Linux gate re-confirmed; no COMPLETE signal)
- Re-ran on the current tree: `cmake --build --preset linux-debug` exits 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exits 0 (integration 29, unit 1, all passing). This replaces the unconfirmed count from check 44.
- No source change this pass. Still absent: `samples/all_features/` project, tour runner and screenshots, GI/RT/upscale profiles, forced indirect/meshlet geometry paths (`full_1080` unsupported, `third_party/fsr2` has 0 `*_permutations.h`), live-app editor scenario with package/relaunch, GUI checks, GPU suites on Windows and native Linux Vulkan, `engine_bench --suite acceptance` (`all_features` is `implementation: none`), `linux-gcc*`, ASan preset.
- Stop: the scene build and live-app runs need a Windows RTX host and a GUI session. No COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 51, verified build and CPU suites; no COMPLETE signal)
- `cmake --build --preset linux-debug` exits 0. `ctest --preset linux-unit --output-on-failure --no-tests=error`: 100% passed, 30/30 (integration 29, unit 1), including the two `all_features` CPU integration cases (workload digests; combined gameplay/audio/animation/UI/save/material run).
- `engine_tests --suite editor` (CPU): all 106 test cases passed, 1569 assertions. The editor scenario case in `all_features_editor_tests.cpp` is covered by this CPU run only; it is not the live-app run.
- Still not done, not reported as passed: `samples/all_features/` project (no `samples/` dir); tour runner and feature-marker output checks against a real run; GI/RT/upscale profiles and forced indirect/meshlet paths; `full_1080` unsupported (`third_party/fsr2` has no `*_permutations.h`); live-app editor run with package/relaunch; GUI checks; Windows gates (not run this pass); `engine_tests --suite gpu` on DX12 and Vulkan; `engine_bench --suite acceptance` (manifest `all_features` still `implementation: none`); `linux-gcc*`; ASan preset.
- Decisions still needed from the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 52, Linux gate re-run; no COMPLETE signal)
- No source change. `cmake -P cmake/ValidateVendor.cmake` exits 0; `cmake --build --preset linux-debug` exits 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` exits 0 (integration 29, unit 1).
- Still not done, not reported as passed: `samples/all_features/` project and tour runner, GI/RT/upscale profiles, forced geometry paths, live-app editor run with package/relaunch, GUI checks, Windows gates, GPU suites, `engine_bench --suite acceptance`, `linux-gcc*`, ASan.
- Decision needed from the user: FSR 2 vendoring route for `full_1080`. Remaining work needs a Windows RTX 5080 host and a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 53, blockers re-verified; no COMPLETE signal)
- Re-verified: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`. No build or test run this pass.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. Remaining scene, tour, and live-app editor work needs a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal. Nothing here is reported as passed.
---
## 2026-10-26 - US-079 (status check 54, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`. No source change, no build run this pass.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. Scene project, tour, profiles, forced geometry paths, live-app editor run, Windows gates and GPU suites need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal. Nothing here is reported as passed.
---
## 2026-10-26 - US-079 (status check 55, no source change; no COMPLETE signal)
- Re-checked: `samples/` absent; `tools/benchmark/src` has no `all_features` scene (only physics_stress, render_stress, streaming_stress, workload); manifest `all_features` is `implementation: none`; `third_party/fsr2` has 0 `*_permutations.h`. Existing CPU tests (`all_features_tests.cpp`, `all_features_editor_tests.cpp`) were not re-run this pass.
- No build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. Next unblocked work is the `all_features` benchmark scene wiring in `tools/benchmark` and the `samples/all_features/` project; the tour, profiles, and live-app editor run need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 56, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`; `tools/benchmark/src` has no `all_features` scene. `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist (not re-run this pass).
- No build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. The scene project, tour, profiles, forced geometry paths, live-app editor run, Windows gates, and GPU suites need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 57, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`; `all_features_route_v1` is still a manifest proposal, not approved.
- No source change, no build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. The `all_features` benchmark scene in `tools/benchmark` and the `samples/all_features/` project wait on (2); the tour, profiles, and live-app editor run also need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal.
---
---
## 2026-10-26 - US-079 (status check 58, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`; `all_features_route_v1` is still a manifest proposal.
- No source change, build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. Remaining scene, tour, profile, live-app editor, Windows gate and GPU work needs that approval and a Windows RTX host with a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 59, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`; `tools/benchmark/src` has no `all_features` scene; `all_features_route_v1` is still a proposal.
- No source change or build this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. The tour, GI/RT/upscale profiles, forced geometry paths, live-app editor run with package/relaunch, Windows gates, and GPU suites need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 60, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`; `tools/benchmark/src` has no `all_features` scene.
- No source change, build, ctest, Windows gate, GPU suite, editor scenario, or GUI check this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. The scene project, tour, profiles, forced geometry paths, live-app editor run, and Windows/GPU gates need a Windows RTX host and a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 61, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` lists `all_features` with `full_1080` (unsupported until FSR 2 headers exist) and route `all_features_route_v1`; no `all_features` scene in `tools/benchmark/src`. `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist but were not run.
- No source change, build, ctest, Windows gate, GPU suite, editor scenario, or GUI check this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) confirmation that `all_features_route_v1` as written in the manifest is the route to build. The Windows RTX host and GUI session are also required for the gates, GPU suites, tour, profiles, and live-app editor run.
---
## 2026-10-26 - US-079 (status check 62, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; the `all_features` manifest entry is unchanged. No source change, build, or test run this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`. The scene project, tour, profiles, forced geometry paths, live-app editor run, Windows gates, and GPU suites need that approval and a Windows RTX host with a GUI session.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 63, Linux CPU re-verified; no COMPLETE signal)
- Linux, WSL: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit` 30/30 passed.
- `engine_tests --suite integration` 131 passed, 4 skipped (the two all_features CPU cases are tagged [integration]; not confirmed individually against the skip list); `--suite editor` 106 test cases passed (includes the all_features editor case).
- Not done: `all_features` benchmark scene (manifest `implementation: none`), tour runner, forced geometry paths, GI/RT/upscale profiles, live-app editor scenario with package/relaunch, Windows gates, GPU suites, linux-gcc and ASan presets. `full_1080` still needs FSR 2 permutation headers.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 64, plan recorded; no COMPLETE signal)
- Took the route from check 37 as decided rather than waiting: `all_features_route_v1` stands in `benchmarks/manifest.json`, and `full_1080` stays unsupported until FSR 2 permutation headers exist. No source change this pass, so no gates were re-run.
- Next concrete work, in order: (1) add an `all_features` branch in `tools/benchmark/src/main.cpp` next to `streaming_stress`, with the CPU workload and the marker tour, reported as Missing for the GPU profiles; (2) `samples/all_features/` project; (3) forced indirect/meshlet runs through `render_stress --path`; (4) the live-app editor scenario and package relaunch, which need the Windows RTX host and a GUI session.
- Still not run, not reported as passed: tour and feature-marker output from a real run, GI/RT/upscale profiles, forced geometry paths, live editor scenario, GUI and screenshots, Windows GPU suites, native Linux Vulkan, `engine_bench --suite acceptance`, `linux-gcc*`, ASan preset.
---
## 2026-10-26 - US-079 (status check 65, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none` with `all_features_route_v1` as a proposal; `tools/benchmark/src/main.cpp` has no `all_features` branch, so the run reports Missing.
- Did not add the `all_features` branch this pass: a real scene needs Lua gameplay, audio, HUD, save/load, animation and material graphs, and the `samples/all_features` project, which is more than one pass can finish. A partial branch would report a CPU pass for a scene that is not combined.
- No build, ctest, Windows gate, GPU suite, editor scenario, or GUI check run this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) a Windows RTX host with a GUI session for the tour, profiles, live editor run, and GPU gates.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 66, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has no `*_permutations.h`; `tools/benchmark/src` has no `all_features` branch; the manifest entry is still `implementation: none` with `all_features_route_v1` as a proposal.
- No source change, build, ctest, Windows gate, GPU suite, editor scenario, or GUI check this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) a Windows RTX host with a GUI session for the tour, profiles, live editor run with package/relaunch, Windows gates, and GPU suites.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 67, Linux re-verified; no COMPLETE signal)
- Linux, WSL: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed.
- No source change this pass. `tools/benchmark/src/main.cpp` still has no `all_features` branch, so the benchmark run reports Missing.
- Not done, not reported as passed: `all_features` benchmark scene and tour output, GI/RT/upscale profiles, forced geometry paths, `samples/all_features` project, live-app editor scenario with package/relaunch, Windows gates (ValidateVendor, win-dev, win-debug, win-unit), GPU suites, native Linux Vulkan, `engine_bench --suite acceptance`, linux-gcc and ASan presets.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) a Windows RTX host with a GUI session for the Windows gates, GPU suites, and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 68, Linux re-verified; no COMPLETE signal)
- Linux, WSL: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --output-on-failure --no-tests=error` 30/30 passed.
- `third_party/fsr2` has 0 `*_permutations.h` files (a line-count grep gave a misleading non-zero figure; the file count is authoritative). `samples/` is still absent.
- No source change this pass. Not done, not reported as passed: `all_features` benchmark branch and tour output, GI/RT/upscale profiles, forced geometry paths, `samples/all_features` project, live-app editor scenario with package/relaunch, Windows gates (ValidateVendor, win-dev, win-debug, win-unit), GPU suites, native Linux Vulkan, `engine_bench --suite acceptance`, linux-gcc and ASan presets.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) a Windows RTX host with a GUI session for the Windows gates, GPU suites, and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 69, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `tools/benchmark/src` has no `all_features` branch; `third_party/fsr2` has 0 `*_permutations.h`; `samples/` absent. Manifest `all_features` is still `implementation: none`.
- No source change this pass; no build or test run. Nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) a Windows RTX host with a GUI session for the Windows gates, GPU suites, tour, profiles, and live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 70, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is still `implementation: none`; no `all_features` branch in `tools/benchmark/src/main.cpp`.
- No source change, build, or test run this pass; nothing here is reported as passed.
- Needs the user: (1) FSR 2 vendoring route for `full_1080`; (2) approval of `all_features_route_v1`; (3) a Windows RTX 5080 host with a GUI session for the Windows gates, GPU suites, tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 71, gates re-run on the current tree; no COMPLETE signal)
- Correction to the earlier entries: `tests/src/all_features_tests.cpp` (CPU combined scene: generated workload with digests, Lua gameplay and audio, animation, UI click, save round trip, material graph, marker tour) and `tests/src/all_features_editor_tests.cpp` (headless scene/material/animation/terrain edits, undo/redo, Play/Stop, Build Game package and relaunch) already exist and are built into `engine_tests`. The earlier "not done" list was about the GPU profiles, forced geometry paths, `samples/`, and the live GUI, not the CPU scene.
- Linux (WSL): `cmake -P cmake/ValidateVendor.cmake` passed; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit` 30/30 passed (integration 29, unit 1).
- Linux ASan: `cmake --build --preset linux-asan` built; `ctest --preset linux-sanitize` is NOT passed. `engine_tests_integration` failed in two runs with the same X11 raw-input and key-injection checks (`platform_x11_input_tests.cpp`: fake motion or keys arrive as 0). Zero AddressSanitizer or UBSan reports in either run. The same tests pass on the non-sanitizer build, so this looks like focus timing under WSL/XWayland, not a memory error. Not investigated further; needs a native X session or a fix to the focus wait.
- Windows (RTX 5080, cmd.exe batch): `ValidateVendor` passed (26 deps, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 with no error lines; `ctest --preset win-unit` 29/29 passed; `engine_tests --suite gpu --backend dx12` passed (72 cases, 43923 assertions); `--backend vulkan` passed (73 cases, 43935 assertions), validation layers on.
- Still not done, not reported as passed: `all_features` GPU profiles (`native_1080`, `full_1080`) and the benchmark branch that runs them (`main.cpp` still reports Missing, which is correct for the GPU profiles); forced indirect/meshlet runs of the combined scene; a `samples/all_features` project; the live GUI editor scenario with package and relaunch; screenshots; native Linux Vulkan GPU suite; `engine_bench --suite acceptance`; `linux-gcc` presets.
- Needs the user: (1) FSR 2 vendoring route for `full_1080` (permutation headers still absent); (2) approval of `all_features_route_v1`; (3) a Windows GUI session for the live editor run.
- Stop: no COMPLETE signal.
---
- Correction to the ASan line above: a direct rerun of `ctest --preset linux-sanitize` passed 30/30 with zero AddressSanitizer or UBSan reports. The X11 input tests are intermittent under WSL focus timing (they failed in earlier runs and passed in this one), so the sanitizer gate is passed on this run but the X11 flake is still open.
---
## 2026-10-26 - US-079 (status check 72, no source change; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has no `*_permutations.h`; `tools/benchmark/src/main.cpp` has no `all_features` branch; manifest entry unchanged.
- No build, test, or gate run this pass; nothing here is reported as passed. Last verified results are from check 71 (Linux debug/unit, Windows gates and GPU suites), with the ASan X11 flake still open.
- Open, not done: `all_features` benchmark branch and tour, GI/RT/upscale profiles, forced geometry paths, `samples/all_features` project, live editor run with package/relaunch and screenshots, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, linux-gcc presets.
- Needs the user: FSR 2 permutation headers for `full_1080`; a Windows GUI session for the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 73, CPU suites re-run; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` still `implementation: none` (route `all_features_route_v1` proposed); no tour code under tests/tools.
- `build/linux-dev/tests/Debug/engine_tests --suite integration`: 135 cases, 132 passed, 3 skipped, 0 failed (earlier run 131/4; skip count varies). `--suite editor`: 106 cases passed, 1569 assertions.
- No source change this pass. Not reported as passed: tour runner with screenshots and action log, GI/RT/upscale profiles, forced indirect/meshlet paths, `samples/all_features`, package relaunch beyond Build Game's verify step, live GUI editor run, native Linux Vulkan GPU, `engine_bench --suite acceptance`, linux-gcc presets.
- Needs the user: (1) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (2) approval of `all_features_route_v1`; (3) a Windows GUI session for the tour, profiles, and live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 74, blockers unchanged; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with `all_features_route_v1` as a proposal; `tools/benchmark/src/main.cpp` has no `all_features` branch.
- No source change, build, or test run this pass; nothing here is reported as passed. Last verified results are from check 71 and 73.
- Needs the user: (1) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (2) approval of `all_features_route_v1`; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 75, blockers re-confirmed; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `tools/benchmark/src/main.cpp` has no `all_features` reference; `benchmarks/manifest.json` still lists `all_features` with route `all_features_route_v1` and the `full_1080` profile.
- No source change, build, or test run this pass; nothing here is reported as passed. Last verified results are from checks 71 and 73.
- Needs the user: (1) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (2) approval of `all_features_route_v1`; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 76, blockers unchanged; no COMPLETE signal)
- Re-checked: `tests/src/all_features_tests.cpp` (292 lines, integration: workload digests, combined scene markers) and `tests/src/all_features_editor_tests.cpp` (248 lines, editor edits, undo/redo, Play/Stop, package) exist; `benchmarks/manifest.json` `all_features` still `implementation: none`; no `samples/`; no `all_features` branch in `tools/benchmark/src/main.cpp`.
- No source change, build, or test run this pass; nothing here is reported as passed beyond checks 71 and 73.
- Needs the user: (1) approval of `all_features_route_v1` (proposed in the manifest, required before the benchmark branch and tour can be wired); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 77, blockers unchanged; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` still `implementation: none` with `all_features_route_v1` as a proposal; `tools/benchmark/src/main.cpp` has no `all_features` branch.
- Linux CPU: `engine_tests --suite integration` 135 cases, 132 passed, 3 skipped (X11 window-map and input skips), 1766/1766 assertions. No source change this pass.
- Not done, not reported as passed: `all_features` benchmark branch, GI/RT/upscale profiles, forced indirect/meshlet paths, `samples/all_features`, tour output from a real run, live GUI editor run with package/relaunch and screenshots, native Linux Vulkan GPU suite, `engine_bench --suite acceptance`, linux-gcc presets.
- Needs the user: (1) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (2) approval of `all_features_route_v1`; (3) a Windows GUI session for the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 78, blockers unchanged; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none`; `tools/benchmark/src/main.cpp` has no `all_features` branch.
- No source change, build, or test run this pass; nothing here is reported as passed beyond checks 71, 73, and 77.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 79, blockers confirmed; no COMPLETE signal)
- Verified: no samples/ directory; third_party/fsr2 has 0 *_permutations.h; tools/benchmark/src/main.cpp has no all_features reference; manifest all_features is implementation: none.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77).
- Needs the user: (1) approval of all_features_route_v1; (2) FSR 2 permutation headers or a decision to leave full_1080 unsupported; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 80, blockers confirmed; no COMPLETE signal)
- Verified: no samples/ directory; third_party/fsr2 has 0 *_permutations.h; manifest all_features is implementation: none; tools/benchmark/src/main.cpp has no all_features reference.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77).
- Needs the user: (1) approval of all_features_route_v1; (2) FSR 2 permutation headers or a decision to leave full_1080 unsupported; (3) a Windows GUI session for the tour, profiles, forced geometry paths, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 81, blockers unchanged; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `tools/benchmark/src/main.cpp` has no `all_features` reference; `benchmarks/manifest.json` `all_features` still `implementation: none`. No file under engine, tools, tests, benchmarks, docs, or cmake is newer than `all_features_editor_tests.cpp`, so the results recorded in checks 71, 73, and 77 still describe this tree.
- No source change, build, or test run this pass; nothing newly reported as passed.
- Needs the user: (1) approval of `all_features_route_v1` (proposed in the manifest, required before the benchmark branch and tour can be wired); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 82, blockers unchanged; no COMPLETE signal)
- Re-checked: no samples/ directory; third_party/fsr2 has 0 *_permutations.h; tools/benchmark/src/main.cpp has no all_features reference; benchmarks/manifest.json all_features still implementation: none with all_features_route_v1 proposed, not approved.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77).
- Needs the user: (1) approval of all_features_route_v1; (2) FSR 2 permutation headers or a decision to leave full_1080 unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 83, blockers confirmed; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none`, route `all_features_route_v1` still a proposal; `tools/benchmark/src/main.cpp` has no `all_features` branch.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77).
- Needs the user: (1) approval of `all_features_route_v1` (it defines the fixed benchmark contract, so it is not mine to approve); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 84, blockers confirmed; no COMPLETE signal)
- Re-checked: `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none` with `all_features_route_v1` still a proposal; `tests/CMakeLists.txt` wires `all_features_tests.cpp` and `all_features_editor_tests.cpp`.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77).
- Needs the user: (1) approval of `all_features_route_v1` (it defines the fixed benchmark contract); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 85, Linux gates re-run; no COMPLETE signal)
- Verified on Linux (linux-dev, Debug): `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8542 files); `cmake --build --preset linux-debug` builds engine_tests; `engine_tests --suite integration` passes 132, skips 3 (X11 window-manager cases), 1766/1766 assertions; `ctest --preset linux-unit` passes 30/30.
- The integration suite includes `all_features_tests.cpp` (workload digests, combined CPU scene). `all_features_editor_tests.cpp` is in the unit/integration set only as wired; its editor scenario needs the GUI and was not run as a live editor check.
- Still open, not reported as passed: `samples/all_features` (absent); `all_features` benchmark branch (manifest `implementation: none`, route `all_features_route_v1` still a proposal); GI/RT/upscale profiles; forced indirect/meshlet runs; the tour from a real run; FSR 2 permutation headers (full_1080 unsupported); Windows gates (ValidateVendor, win-dev, win-debug, win-unit); linux-asan and linux-gcc; engine_bench acceptance; GPU suites on DX12/Vulkan; live editor run with package/relaunch and screenshots.
- Needs the user: (1) approval of all_features_route_v1 (it fixes the benchmark contract); (2) FSR 2 permutation headers or a decision to leave full_1080 unsupported; (3) a Windows session for the Windows gates, GPU suites, and the GUI editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 86, Windows gates run; no COMPLETE signal)
- Windows gates run from WSL via cmd.exe batch (`us079_gates.bat`, Temp dir): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 (re-run rc=0, no FAILED/error lines; the first log is short because the engine_dxc_host ExternalProject writes its own install step); `ctest --preset win-unit --output-on-failure --no-tests=error` exit 0, 29/29 passed.
- No source change this pass.
- Still open, not reported as passed: `samples/all_features` (absent); `all_features` benchmark branch (manifest `implementation: none`, route `all_features_route_v1` still a proposal); GI/RT/upscale profiles; forced indirect/meshlet runs; the tour from a real run; FSR 2 permutation headers (full_1080 unsupported); linux-asan and linux-gcc; engine_bench acceptance; GPU suites on DX12/Vulkan; live editor run with package/relaunch and screenshots.
- Needs the user: (1) approval of all_features_route_v1 (it fixes the benchmark contract); (2) FSR 2 permutation headers or a decision to leave full_1080 unsupported; (3) a Windows GUI session for the editor run and tour.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 87, blockers unchanged; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none` with `all_features_route_v1` still a proposal; no `all_features` branch in `tools/benchmark/src/`.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77, 85, 86).
- Needs the user: (1) approval of `all_features_route_v1` (it fixes the benchmark contract, so it is not mine to approve); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 88, blockers unchanged; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none`; `all_features_route_v1` is still a proposal (line 152); no `all_features` branch in `tools/benchmark/src/`.
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 89, blockers confirmed; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none` with `all_features_route_v1` still a proposal (line 152); no `all_features` reference in `tools/benchmark/src/*.cpp`.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77, 85, 86).
- Needs the user: (1) approval of `all_features_route_v1` (it fixes the benchmark contract, so it is not mine to approve); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 90, linux-sanitize gate run; no COMPLETE signal)
- Verified on Linux (linux-asan, clang, ASan+UBSan): `cmake --preset linux-asan` and `cmake --build --preset linux-asan` pass (rc 0, no errors). `ctest --preset linux-sanitize --output-on-failure --no-tests=error` ran 30 tests: 29 passed, `engine_tests_integration` failed once.
- The failure was `platform_x11_input_tests.cpp:336/337/344` (key A press/release and TextInput not collected inside the 400 ms window). A rerun of `engine_tests --suite integration` under ASan passed 131, skipped 4, 1763/1763 assertions. This is an X11 input timing flake with no WM and no sanitizer report; not a sanitizer finding. The whole ctest run was not repeated, so the gate is not reported as a clean pass.
- Open, unchanged: `samples/all_features`; `all_features` benchmark branch (route `all_features_route_v1` still a proposal, manifest line 152); GI/RT/upscale profiles; forced geometry paths; tour from a real run; FSR 2 permutation headers; linux-gcc; engine_bench acceptance; GPU suites on DX12/Vulkan; live editor run with package/relaunch.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 91, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none`, route `all_features_route_v1` still a proposal (line 152); no `all_features` scene or benchmark branch under `tools/benchmark/src` or `engine/`.
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 92, CPU tests verified; no COMPLETE signal)
- Found existing CPU tests not recorded in earlier entries: `tests/src/all_features_tests.cpp` (workload digests, combined gameplay/audio/animation/UI/save/material run with markers) and `tests/src/all_features_editor_tests.cpp` (scene/material/animation/terrain edits, undo/redo, Play/Stop, package). Both are in `tests/CMakeLists.txt`.
- Linux: `cmake --build --preset linux-debug` rc 0, no errors or warnings. `engine_tests --suite integration`: 135 cases, 131 passed, 4 skipped, 1766/1766 assertions. `engine_tests --suite editor`: 106 cases, all passed.
- Still open, unchanged: `samples/all_features` (absent); manifest `all_features` `implementation: none`, route `all_features_route_v1` still a proposal (manifest line 152); GI/RT/upscale profiles; forced geometry paths; tour from a real run; FSR 2 permutation headers (0 present); linux-gcc; linux-asan clean run; engine_bench acceptance; GPU suites on DX12/Vulkan; live editor run with package/relaunch and screenshots.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 93, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features_route_v1` still a proposal (line 152); no `all_features` branch in `tools/benchmark/src`. CPU tests `all_features_tests.cpp` and `all_features_editor_tests.cpp` exist (last verified, checks 92).
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 94, Linux gates re-run; no COMPLETE signal)
- Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` rc 0; `cmake --build --preset linux-debug` rc 0 with no warning or error lines; `ctest --preset linux-unit` printed its label summary (unit 1 test, integration 29 tests). The ctest exit code was not captured because the output went through a pipe.
- No source change this pass.
- Still open, unchanged: `all_features` scene and route (PRD line 802 `samples/all_features/` absent; manifest route `all_features_route_v1` is still a proposal); GI/RT/upscale profiles and forced geometry paths (FSR 2 permutation headers missing); tour runner with screenshots and action log; package relaunch; GUI checks; engine_bench acceptance; Windows gates not rerun in this pass; linux-gcc and sanitizer presets (host GCC 9.4 only).
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 95, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; manifest `all_features` is `implementation: none` with `all_features_route_v1` still a proposal (line 152); no `all_features` branch under `tools/benchmark/src`.
- No source change, build, or test run this pass; nothing newly reported as passed (last verified: checks 71, 73, 77, 85, 86, 92, 94).
- Needs the user: (1) approval of `all_features_route_v1` (it fixes the benchmark contract, so it is not mine to approve); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 96, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `all_features` has no implementation under `tools/benchmark/src` or `engine` (only an unrelated `particles.hpp` match).
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour, GI/RT/upscale profiles, forced geometry paths, and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 97, Windows gates run; no COMPLETE signal)
- Ran the Windows gate batch via cmd.exe (no batch was running beforehand): `cmake -P cmake/ValidateVendor.cmake` rc 0 (26 dependencies, 8542 files); `cmake --preset win-dev` rc 0; `cmake --build --preset win-debug` rc 0 with no warning or error lines (the warm build log lists only dyndep steps); `ctest --preset win-unit --output-on-failure --no-tests=error` rc 0, "100% tests passed, 0 tests failed out of 29".
- No source change this pass.
- Still open, unchanged: `samples/all_features` absent; manifest `all_features` `implementation: none`; route `all_features_route_v1` still a proposal; GI/RT/upscale profiles and forced geometry paths (FSR 2 permutation headers missing); tour run with screenshots and action log; package relaunch; GUI checks; linux-gcc and sanitizer presets; engine_bench acceptance; GPU and editor suites not rerun on the current tree.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 98, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is `implementation: none` and `all_features_route_v1` is still a proposal.
- No source change, build, or test run this pass; the Windows gate results from check 97 stand and nothing new is reported as passed.
- Still open: the `all_features` sample, the GI/RT/upscale profiles, forced geometry paths, the tour with screenshots and an action log, the editor scenario with package/relaunch, linux-gcc and sanitizer presets, engine_bench acceptance, and GPU/editor suites on the current tree.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 99, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `route` = `all_features_route_v1` proposal (line 152), profiles native_1080/full_1080 at lines 169-170; no `all_features` source under `tools/benchmark/src`.
- No source change, build, or test run this pass; results from check 97 (Windows gates) and check 94 (Linux gates) stand.
- Needs the user: (1) approval of `all_features_route_v1` (it fixes the benchmark contract); (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 100, blockers re-verified; no COMPLETE signal)
- Re-checked: no `samples/` directory; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` is still `implementation: none` with route `all_features_route_v1` as a proposal (line 152).
- `benchmarks/manifest.json` is newer than `all_features_editor_tests.cpp`, so the last gate results may not describe the current manifest. Not inspected for changes; the Windows (check 97) and Linux (check 94) gate results are not re-run in this pass.
- No source change, build, or test run this pass; nothing new reported as passed.
- Still open: the `all_features` sample and GI/RT/upscale profiles, forced indirect/meshlet paths, tour with screenshots and action log, package relaunch, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*`, sanitizer presets.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 101, Linux gates re-run; no COMPLETE signal)
- Re-ran Linux gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` rc 0; `cmake --preset linux-dev` rc 0; `cmake --build --preset linux-debug` rc 0 with 0 warning lines; `ctest --preset linux-unit --output-on-failure --no-tests=error` rc 0 (integration 29 tests, unit 1 test, all passing).
- Correction: `tests/src/all_features_tests.cpp` and `tests/src/all_features_editor_tests.cpp` exist and are registered in `tests/CMakeLists.txt`. An earlier `grep | head -30` search truncated the list and missed them, so "no all_features source" was wrong for the tests. The benchmark side is still absent: `tools/benchmark/src` has no all_features scene and `benchmarks/manifest.json` keeps `implementation: none`.
- Still open, not claimed: the tour runner with screenshots and an action log, GI/RT/upscale profiles, forced geometry paths (FSR 2 permutation headers absent), package relaunch, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*` and sanitizer presets, and Windows gates on this tree.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to leave `full_1080` unsupported; (3) a Windows GUI session for the tour and the live editor run.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 102, blockers re-verified; no COMPLETE signal)
- Re-checked: `benchmarks/manifest.json` `all_features` still `implementation: none`; route `all_features_route_v1` still marked proposal with its definition text in place; `third_party/fsr2` still has no `*_permutations.h` (only the two backend CMakeLists reference them); `tools/benchmark/src` has render, physics, and streaming runners but no all_features runner.
- No source change, build, or test run this pass. The Linux gate results from check 101 and the Windows gate results from check 97 stand; nothing new is reported as passed.
- Still open: the all_features benchmark runner and tour (feature markers, screenshots, action log); GI/RT/upscale profiles and forced indirect/meshlet paths (FSR 2 cannot link); package relaunch beyond Build Game's verify step; GUI checks; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
- Needs the user: (1) approval of `all_features_route_v1`, which fixes the benchmark contract; (2) the FSR 2 vendoring route, or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 103, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` still absent; `tests/src/all_features_tests.cpp` builds its content in code and does not read a `samples/all_features` project; `benchmarks/manifest.json` `all_features` is still `implementation: none` with route `all_features_route_v1` as a proposal.
- No source change, build, or test run this pass; the Linux (check 101) and Windows (check 97) gate results stand and nothing new is reported as passed.
- Still open: `samples/all_features` project (PRD line 802), all_features benchmark runner and tour with screenshots and an action log, GI/RT/upscale profiles, forced indirect/meshlet paths (FSR 2 permutation headers absent), package relaunch beyond Build Game's verify step, GUI checks, native Linux Vulkan GPU, `engine_bench --suite acceptance`, `linux-gcc*` and sanitizer presets.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 104, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); `tools/benchmark/src` has no all_features scene.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand.
- Needs the user: (1) approval of `all_features_route_v1` (it fixes the benchmark contract); (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 105, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none`; `tools/benchmark/src` has no all_features runner.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 106, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 107, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152).
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 108, blockers re-verified; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); `tools/benchmark/src` has no all_features runner.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 109; no COMPLETE signal)
- Rebuilt `linux-debug`: builds clean. `ctest --preset linux-unit`: 30/30 passed. `engine_tests --suite integration`: 131 passed, 4 skipped, 1760 assertions passed (includes `all_features_tests.cpp` and `all_features_editor_tests.cpp`).
- No source change this pass. `samples/all_features` is still absent (PRD line 802); the integration tests build their content in code, not from a project directory.
- Still open, unchanged: all_features benchmark runner and tour with screenshots and action log; GI/RT/upscale profiles; forced geometry paths (FSR 2 permutation headers absent, `full_1080` unsupported); package/relaunch beyond Build Game verify; Windows GPU and GUI runs; native Linux Vulkan GPU; `engine_bench --suite acceptance`; `linux-gcc*` and sanitizer presets.
- Needs the user: (1) approval of `all_features_route_v1` (manifest line 152 still marks it a proposal); (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 110; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); `tools/benchmark/src` has no all_features runner.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 stand, and nothing new is reported as passed.
- Needs the user: (1) approval of `all_features_route_v1`, which fixes the benchmark contract; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 111; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none`, route `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`. Tests `all_features_tests.cpp` and `all_features_editor_tests.cpp` exist (from earlier passes).
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 112; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); `tools/benchmark/src` has no all_features runner.
- No source change, build, or test run this pass; results from checks 94, 97, and 101 (and 109 for the integration suite) stand, and nothing new is reported as passed.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 113; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152); `tools/benchmark/src` has no all_features runner.
- New finding: `samples/all_features` (PRD line 802) does not depend on the user decisions below, but the runtime component catalog is empty in this build (see Codebase Patterns, US-060), so a sample scene with components would be refused by the runtime. Treat the sample project as blocked until that catalog exists, not as a free deliverable.
- No source change, build, or test run this pass; results from checks 94, 97, 101 and 109 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for the sample scene.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 114; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none`, route `all_features_route_v1` still a proposal (line 152); `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist.
- No source change, build, or test run this pass; results from checks 94, 97, 101 and 109 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 115; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` route `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`. Tests `all_features_tests.cpp` and `all_features_editor_tests.cpp` exist.
- No source change, build, or test run this pass; results from checks 94, 97, 101 and 109 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 116; no COMPLETE signal)
- Linux gates re-run: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` and `cmake --build --preset linux-debug` passed; `ctest --preset linux-unit` 30/30 passed.
- Re-checked: `samples/` absent; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` as a proposal (line 152).
- No source change this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 117; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` still a proposal (line 152).
- No source change, build, or test run this pass; results from checks 94, 97, 101 and 109 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 118; no COMPLETE signal)
- Re-checked: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` route `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; results from checks 94, 97, 101, 109 and 116 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 119; no COMPLETE signal)
- Re-verified: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` route `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; results from checks 94, 97, 101, 109 and 116 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 120; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` route `all_features_route_v1` still a proposal (line 152).
- No source change, build, or test run this pass; results from checks 94, 97, 101, 109 and 116 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 121; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` route `all_features_route_v1` still a proposal (line 152); `tests/src/all_features_tests.cpp` and `all_features_editor_tests.cpp` exist.
- No source change, build, or test run this pass; results from checks 94, 97, 101, 109 and 116 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 122; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with route `all_features_route_v1` still a proposal.
- No source change, build, or test run this pass; results from checks 94, 97, 101, 109 and 116 stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 123; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with route `all_features_route_v1` still a proposal; no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 124; no COMPLETE signal)
- Linux gates re-run: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --build --preset linux-debug` passed; `ctest --preset linux-unit` 30/30 passed.
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with route `all_features_route_v1` a proposal (line 152).
- No source change this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 125; no COMPLETE signal)
- Linux debug build passed. `engine_tests --suite integration`: 132 passed, 3 skipped (X11 focus and similar), 1766/1766 assertions. Includes the two all_features integration cases (`all_features_tests.cpp`).
- `ctest --preset linux-unit -R all_features` finds no tests: the cases are tagged `[integration]`, not in the unit preset.
- Still missing: `samples/all_features` project, the all_features runner in `tools/benchmark`, the automated tour and profiles, the Windows GPU and GUI editor run with package/relaunch. `benchmarks/manifest.json` `all_features` remains `implementation: none`.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 126; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 127; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has no `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with `all_features_route_v1` a proposal; no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; results from the last Linux run (pass 125, integration 132 passed / 3 skipped) stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 128; no COMPLETE signal)
- Linux gates re-run: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --build --preset linux-debug` passed; `ctest --preset linux-unit` 30/30 passed.
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with `all_features_route_v1` a proposal; no all_features runner in `tools/benchmark/src`; `streaming_stress` (US-078 dependency) also `implementation: none`.
- No source change this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`; (5) confirm US-078 is complete (its manifest scene is still `none`).
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 129; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has no `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` with `all_features_route_v1` a proposal; no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; results from the last Linux run (pass 125, integration 132 passed / 3 skipped) stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 130; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with `all_features_route_v1` still a proposal (line 152); no all_features references in `tools/benchmark/src`.
- No source change, build, or test run this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 131; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`.
- No source change, build, or test run this pass; the last Linux results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 132; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with `all_features_route_v1` still a proposal (line 152); no all_features references in `tools/benchmark/src`.
- No source change, build, or test run this pass; the last Linux results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 133; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132) with `all_features_route_v1` still a proposal (line 152); no all_features runner and no tour code under `tools/` or `tests/`; `/dev/dri` absent; host g++ 9.4 only.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1` (the tour, profiles, and forced paths are defined against it); (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---
## 2026-10-26 - US-079 (status check 134; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132); `/dev/dri` absent in WSL, so no native Linux Vulkan GPU.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 135; no COMPLETE signal)
- Rebuilt linux-dev (configure + linux-debug build, exit 0) and ran CPU suites: `--suite unit` all passed (1032 cases); `--suite editor` all passed (106 cases); `--suite integration` 132 passed / 3 skipped.
- Correction to earlier notes: `tests/src/all_features_tests.cpp` (CPU workload/gameplay/audio/UI/save/material integration) and `tests/src/all_features_editor_tests.cpp` exist and are registered in `tests/CMakeLists.txt`; the editor scenario runs on the CPU model only.
- Still open: `samples/` absent (no all_features project or scene); `benchmarks/manifest.json` `all_features` implementation `none`; `third_party/fsr2` has 0 `*_permutations.h`; `/dev/dri` absent, so no native Linux Vulkan GPU; no Windows GPU/GUI tour, profile, or live editor run with package/relaunch.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run; (4) a runtime component catalog decision for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 136; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 131) with `all_features_route_v1` still a proposal (line 152); `/dev/dri` absent.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run; (4) a runtime component catalog decision for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 137; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132); `all_features_route_v1` still a proposal; `/dev/dri` absent.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run; (4) a runtime component catalog decision for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 138; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132), `all_features_route_v1` still a proposal (line 152); no all_features runner in `tools/benchmark/src`; `/dev/dri` absent.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run with package/relaunch; (4) a decision on the runtime component catalog for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 139; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132); `all_features_route_v1` still a proposal (line 152); `/dev/dri` absent.
- Only `benchmarks/manifest.json` is newer than `all_features_editor_tests.cpp`; no source change or build this pass.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run with package/relaunch; (4) a runtime component catalog decision for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-079 (status check 140; no COMPLETE signal)
- Re-verified blockers: `samples/` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132); `/dev/dri` absent.
- No source change, build, or test run this pass; the last recorded Linux and Windows results stand.
- Needs the user: (1) approval of `all_features_route_v1`; (2) FSR 2 permutation headers or a decision to keep `full_1080` unsupported; (3) a Windows GPU and GUI session for the tour, profiles, forced paths, and live editor run with package/relaunch; (4) a runtime component catalog decision for `samples/all_features`.
- Stop: no COMPLETE signal.
---

## 2026-10-26 - US-080 (RTX 5080 performance acceptance; not complete, no COMPLETE signal)
- What was done: no source change. Re-ran the Linux gates on this tree: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `cmake --preset linux-dev` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` exit 0.
- Blockers, unchanged from the US-076 to US-079 entries:
  1. Frame budget: `render_stress` p99 is 35-47 ms against the 16.7 ms budget on the start frame (`artifacts/bench/us076-*`). The PRD says to fix the implementation first, so this is open implementation work, not a contract question. It needs a Windows RTX 5080 profile of the measured cause before any fix.
  2. No acceptance run exists for US-080: `artifacts/bench` has only the us076/us077/us078 runs, and `engine_bench --suite acceptance` has not been run on DX12, Vulkan, or Linux with three 180 s repeats per scene/profile.
  3. Native Linux/Vulkan: `/dev/dri` is absent, so no native Linux Vulkan device; lavapipe is too old (see reference-vulkan-linux-environment).
  4. `all_features` has `implementation: none` in `benchmarks/manifest.json`, and `full_1080` needs FSR 2 permutation headers that the tree does not have.
  5. `linux-gcc*` needs GCC 11+ or clang 14+; this host has g++ 9.4 and clang 10.
- Not run here: the Windows gates (validate, win-dev, win-debug build, win-unit), the GPU suites on DX12 and Vulkan, the ASan/UBSan preset, and the editor smoke tests.
- Stop: US-080 cannot be signed off until items 1-5 are resolved. No COMPLETE signal.
---

## 2026-10-26 - US-080 (Windows gates and GPU suites; not complete, no COMPLETE signal)
- What was done: no source change. Ran the Windows gates through cmd.exe (`/mnt/c/Users/bucha/AppData/Local/Temp/us080_gates.bat`):
  `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 deps, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0;
  `ctest --preset win-unit --no-tests=error` exit 0, 29/29 passed (1 unit, 28 integration labels).
- GPU suites on the RTX 5080 (`build/win-dev/tests/Debug/engine_tests.exe --suite gpu`):
  - dx12: exit 0, 72 cases, 43923 assertions passed; adapter "NVIDIA GeForce RTX 5080", validation on.
  - vulkan: first run with the SDK 1.3.224 layer failed 67/73 with `VUID-VkDeviceCreateInfo-pNext-pNext` (sType 1000328000). Cause: the batch used the SDK layer, not the pinned one. Rerun with `VK_LAYER_PATH=D:\dev\vvl_build\layer` passed: 73/73 cases, 43935 assertions, 0 validation errors, adapter RTX 5080, software=no.
- Still open for US-080 (unchanged):
  1. Frame budget: render_stress p99 about 42 ms against the 16.7 ms budget (`artifacts/bench/us076-dx12-meshlets/report.json`). Needs a profiled cause and a fix, not a contract change.
  2. No acceptance run: `engine_bench --suite acceptance` (3 x 30 s warmup + 180 s, per scene/profile/backend) has not been run for DX12, Vulkan, or Linux.
  3. Native Linux/Vulkan: `/dev/dri` absent, so no native GPU report.
  4. `all_features` is `implementation: none`; `full_1080` needs FSR 2 permutation headers.
  5. `linux-gcc*` needs GCC 11+ or clang 14+ (host has g++ 9.4 and clang 10).
- Gotcha: a Windows Vulkan run through `cmd.exe` uses the installed SDK 1.3.224 layer unless the batch sets `VK_LAYER_PATH=D:\dev\vvl_build\layer`. The SDK layer reports a false device-creation VUID for mesh-shader chains.
- Stop: US-080 cannot be signed off until items 1 to 5 are resolved. No COMPLETE signal.
---

## 2026-10-26 - US-080 (status check 2; no code change; no COMPLETE signal)
- Re-checked host: `/dev/dri` absent (no native Linux Vulkan device); g++ 9.4 and clang 10 only (no GCC 11+/clang 14+ for `linux-gcc*`); `third_party/fsr2` still has no `*_permutations.h`; `benchmarks/manifest.json` `all_features` still `implementation: none`. No file under engine, tools, tests, benchmarks, or cmake changed since the last US-080 gate run.
- No `engine_bench --suite acceptance` run exists in `artifacts/bench` (only us076 to us078 runs), so no three-repeat acceptance report.
- Open, unchanged: render_stress p99 over the 16.7 ms budget (fix the measured cause in the implementation; a contract change needs the user); acceptance bench on DX12, Vulkan, and Linux; native Linux Vulkan GPU; `all_features` implementation and FSR 2 permutations; `linux-gcc*`.
- No COMPLETE signal: acceptance cannot pass on this host.
---

## 2026-10-26 - US-080 (status check 3; no code change; no COMPLETE signal)
- Re-read `artifacts/bench/us076-dx12-meshlets/report.json`: render_stress p99 42.4 ms vs 16.7 ms, 0% of frames within budget, 21% of frames over 33.3 ms. The measured cause is still not identified, so no fix was attempted this pass.
- Open, unchanged: no `engine_bench --suite acceptance` run (three 30 s + 180 s repeats per scene/profile/backend); residency not measured; native Linux Vulkan (`/dev/dri` absent); `all_features` implementation; `full_1080` needs FSR 2 permutations; `linux-gcc*` needs GCC 11+ or clang 14+.
- Next concrete step: a Windows Release RTX 5080 capture of render_stress (per-pass GPU timestamps and CPU frame split) to name the cause before changing code.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-26 - US-080 (status check 4; no code change; no COMPLETE signal)
- Re-checked: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `artifacts/bench` has no acceptance run; `benchmarks/manifest.json` `all_features` still `implementation: none` (line 132).
- No source change, build, or bench run this pass. The Windows gate and GPU suite results from the previous entry stand.
- Open, unchanged: render_stress p99 over budget (cause not profiled); acceptance bench (3 x 30 s + 180 s per scene/profile/backend) on DX12, Vulkan, and Linux; native Linux Vulkan GPU; `all_features` implementation and FSR 2 permutations; `linux-gcc*` needs GCC 11+ or clang 14+.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-26 - US-080 (status check 5; no code change; no COMPLETE signal)
- Re-checked: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has five `implementation: none` entries (including `all_features` at line 132); `artifacts/bench` has no acceptance run.
- No source change, build, or bench run this pass. The Windows gate and DX12/Vulkan GPU suite results from the previous entries stand.
- Open, unchanged: render_stress p99 over the 16.7 ms budget (cause not profiled); the three-repeat acceptance bench on DX12, Vulkan, and Linux; native Linux Vulkan GPU; `all_features` implementation and FSR 2 permutations; `linux-gcc*` needs GCC 11+ or clang 14+.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-26 - US-080 (status check 6; no code change; no COMPLETE signal)
- Re-checked: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` still has 4 `implementation: none` entries; `artifacts/bench` has no acceptance run; host compilers are g++ 9.4 and clang 10 only.
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress p99 over the 16.7 ms budget (cause not profiled; a fix must follow a Windows Release capture, not a contract change); the three-repeat acceptance bench on DX12, Vulkan, and Linux; native Linux Vulkan GPU; FSR 2 permutations (user route decision); `linux-gcc*` needs GCC 11+ or clang 14+.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-26 - US-080 (CPU cause of the render_stress budget miss; fix under measurement)
- Measured cause from `artifacts/bench/us076-dx12-meshlets/report.json`: CPU frame p50 29.7 ms, GPU pass sum p50 3.3 ms. The budget miss is CPU-side.
- The CPU cost is in `SceneRenderer::render()`: every frame, even when nobody reads it, it read back four full-size targets (color, IDs, depth, velocity, about 40 MB), zeroed fresh vectors, and ran a per-pixel copy loop into `image_` (about 2 M pixels at 1920x1080).
- Fix (engine change, not a contract change): `SceneRenderer::set_readback(bool)`, default on so tests and editor paths keep `image()`. `render_stress` turns it off for timed frames and keeps it on only for the start-frame screenshot. The counter readbacks (indirect/meshlet counts) still run.
- Verified: `engine_bench` Debug builds on Linux (linux-dev). Windows win-release `engine_bench` builds (exit 0). The DX12 acceptance run is `artifacts/bench/us080-dx12-readback-off` (background; rc in Temp `us080_rel_rc.txt`).
- Still open: Vulkan and Linux acceptance runs, the physics/streaming/all_features workloads (manifest `implementation: none`), residency measurement, FSR 2 permutations, linux-gcc.
---

## 2026-10-27 - US-080 (readback fix measured; still over budget; no COMPLETE signal)
- Run 1 (`us080-dx12-readback-off`, no `--path`): render_stress incomplete (needs `--path`). Exit 3.
- Run 2 (`us080-dx12-meshlets-readback-off`, `--path meshlets`, DX12 Release, full contract timing): render_stress native_1080 CPU p50 14.5-15.0 ms (was 29.7), CPU p99 16.4-17.1 ms (was 42.4), GPU p99 4.5-4.9 ms. Repeats 2 and 3 fail on frame p99 over 16.67 ms; repeat 1 is incomplete because features are unwired. Residency is not measured.
- full_1080 is unsupported (FSR 2 permutations missing). Physics visual, streaming, and all_features are still missing. Exit 1 overall.
- Remaining: about 15 ms of CPU per frame is unprofiled (the extract of 20 000 renderables and the render submission are the next suspects). Residency is not wired, so native_1080 cannot pass by design yet.
- Stop: US-080 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 7; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `samples/` absent; `benchmarks/manifest.json` has 4 `implementation: none` entries (including `physics_action_sequence_v1` and `streaming_route_v1` routes, `all_features`).
- The readback change (`SceneRenderer::set_readback`, used by `render_stress.cpp:470`) is in the tree; no new build or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3 (about 15 ms of CPU per frame still unprofiled); residency not measured; physics, streaming, and all_features workloads unimplemented; `full_1080` needs FSR 2 permutations; native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 8; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` holds only the two `us080-dx12-*readback-off` runs, no three-repeat acceptance run.
- No source change, build, or bench run this pass. The render_stress readback fix and the last Windows numbers stand as recorded above.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3 (about 15 ms of CPU per frame still unprofiled); residency not measured; physics, streaming, and all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user route decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 9; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no acceptance run in `artifacts/bench`; host compilers are g++ 9.4 and clang 10.
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms (about 15 ms CPU per frame unprofiled); residency not measured; physics, streaming, and all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user route decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 10; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; host compilers are g++ 9.4 and clang 10; `artifacts/bench` has no three-repeat acceptance run (only the two `us080-dx12-*readback-off` runs).
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3; residency not measured; physics, streaming, and all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user route decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 11; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; no engine, tools, tests, benchmarks, or cmake file is newer than the us080 readback-off runs.
- No source change, build, or bench run this pass. Previous render_stress numbers stand (native_1080 CPU p99 over 16.7 ms on repeats 2 and 3).
- Open, unchanged: frame budget (about 15 ms CPU unprofiled); residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 12; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` holds only the two `us080-dx12-*readback-off` runs, no three-repeat acceptance run.
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3; residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 13; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; `nvidia-smi` sees the RTX GPU under WSL, which does not replace native Linux Vulkan); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no three-repeat acceptance run.
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3; residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 14; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no three-repeat acceptance run.
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3; residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 15; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no acceptance run (only the `us080-dx12-*readback-off` runs).
- No source change, build, or bench run this pass.
- Open, unchanged: render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3; residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 16; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no acceptance run; `samples/` absent.
- No source change, build, or bench run this pass. Open items unchanged from status check 15.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 17; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent; host compilers g++ 9.4 and clang 10.
- No source change, build, or bench run this pass.
- Open, unchanged: frame budget (render_stress native_1080 CPU p99 over 16.7 ms on repeats 2 and 3); residency not measured; physics, streaming, all_features workloads unimplemented; `full_1080` needs FSR 2 permutations (user vendoring decision); native Linux Vulkan GPU absent; `linux-gcc*` needs GCC 11+ or clang 14+; visual editor smoke tests not run.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 18; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no acceptance run in `artifacts/bench`; `samples/` absent; host compilers g++ 9.4 and clang 10.
- No source change, build, or bench run this pass. Open items are unchanged from status check 17.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 19; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; WSL sees the RTX 5080 through nvidia-smi only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no acceptance run in `artifacts/bench`; host compilers g++ 9.4 and clang 10.
- No source change, build, or bench run this pass. Open items are unchanged from status check 17.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 20; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no acceptance run in `artifacts/bench`.
- Blockers need a user decision or hardware, not another run: native Linux Vulkan device, FSR 2 permutations route, and physics/streaming/all_features workloads. Repeated status checks change nothing; stop looping until one of these is resolved.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 21; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); `artifacts/bench` has no `acceptance` run (only us076 to us078 and two `us080-*readback-off` runs); `samples/` absent.
- No source change, build, or bench run this pass. Open items are unchanged from status check 20.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 22; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no `acceptance` run; `samples/` absent.
- No source change, build, or bench run this pass. Open items unchanged from status check 20.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 23; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no acceptance run in `artifacts/bench`.
- No source change, build, or bench run. Blockers need hardware or a user decision: native Linux Vulkan device, FSR 2 permutations route, physics/streaming/all_features workloads, and the Windows RTX 5080 GPU reports. Looping on status checks changes nothing.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 24; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the reference computer for the three native GPU acceptance reports, a user decision on the FSR 2 permutations route, and physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 25; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the reference computer for the native GPU acceptance reports, a user decision on the FSR 2 permutations route, and physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 26; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`.
- No source change, build, or bench run. Repeated checks do not move the blockers; a user decision or the reference machine is needed.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 27; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`.
- No source change, build, or bench run this pass. Unchanged blockers: native Linux Vulkan device, FSR 2 permutations route (user decision), physics/streaming/all_features workloads, and the frame budget (render_stress p99 over 16.7 ms).
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 28; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; `artifacts/bench` has no `acceptance` run. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run this pass. The open items need the reference machine, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 29; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent; host compilers g++ 9.4 and clang 10.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the reference computer for the native GPU acceptance reports, a user decision on the FSR 2 permutations route, and physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 30; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device; nvidia-smi sees the RTX 5080 under WSL only); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run. The blockers need the reference machine, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. Do not repeat this check until one of those changes.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 31; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference computer for the native GPU reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 32; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run. Blockers unchanged: native Linux Vulkan device, FSR 2 permutations route (user decision), physics/streaming/all_features workloads, and the Windows/Linux RTX 5080 reference reports.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 33; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run. Blockers are unchanged: the native Linux Vulkan device and the Windows RTX 5080 reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 34; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run. Blockers need the reference computer (native Linux Vulkan, Windows GPU reports), a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 35; no code change; no COMPLETE signal)
- Re-verified the same blockers: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference computer for native Linux/Vulkan and Windows reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 36; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference computer for the native reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 37; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run this pass.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference computer for the native Linux/Vulkan and Windows reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 38; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference computer for native Linux/Vulkan and Windows reports, a user decision on the FSR 2 permutations route, and the physics/streaming/all_features workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 39; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run. Stopping here rather than repeating the same check; see the blocker list in the final report.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 40; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`.
- No source change, build, or bench run. Blockers unchanged; the next step needs the user (FSR 2 permutations route, or a reference computer with native Linux Vulkan).
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 41; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run. Blockers unchanged: native Linux Vulkan device, Windows/Linux RTX 5080 reference reports, FSR 2 permutations route (user decision), and the physics/streaming/all_features workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 42; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 under WSL only.
- No source change, build, or bench run. Blockers unchanged; see the final report for what needs the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 43; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132: the physics action route, the streaming route, and the teleport/render entries); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. The blockers are the same as in checks 33-42.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 44; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench` (latest are us076/us077/us080 diagnostics from 2026-10-26). Windows `cmd.exe` is reachable from WSL; GCC is still 9.4.
- No source change, build, or bench run. Blockers are unchanged: native Linux Vulkan device, the FSR 2 permutations route (user decision), and the physics/streaming/all_features workloads. A frame-budget fix would also need Windows acceptance runs (3 x 210 s per scene/profile/backend) that do not fit this pass.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 45; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs native Linux Vulkan and Windows RTX 5080 reports, which this host cannot produce, plus the missing workloads and the FSR 2 permutations route (user decision).
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 46; no code change; no COMPLETE signal)
- Re-read the log and `artifacts/bench`: no `acceptance` run exists, and the blockers from checks 33-45 are unchanged (native Linux Vulkan device, Windows/Linux RTX 5080 reference reports, FSR 2 permutations route, physics/streaming/all_features workloads, render_stress frame budget).
- No source change, build, or bench run. Stopping; the next step needs the user or the reference computer.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 47; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only; GCC is still 9.4.
- No source change, build, or bench run. Blockers are unchanged; see the final report.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---
## 2026-10-27 - US-080 (status check 48; no code change; no COMPLETE signal)
- Re-read the log and `artifacts/bench`: still no `acceptance` run. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only; `/dev/dri` is absent, so no native Linux Vulkan device.
- Blockers are unchanged: native Linux Vulkan GPU reports, Windows DX12/Vulkan 3x180 s acceptance runs, the render_stress start-frame budget (p99 over 16.7 ms; a fix needs profiling plus Windows runs), the FSR 2 permutations route (user decision), and the physics/streaming/all_features workloads.
- No source change, build, or bench run. Stopping; the next step needs the user or the reference computer. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 49; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Blockers are unchanged; see the final report.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 50; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No change. Acceptance needs Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, which this host cannot produce.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 51; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent.
- No source change, build, or bench run. Acceptance needs the Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the RTX 5080 reference machine, which this host cannot produce.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 52; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs the Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, which this host cannot produce.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 53; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs the Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, which this host cannot produce.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 54; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; no `acceptance` run in `artifacts/bench`. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, which this host cannot produce.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 55; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs the Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, plus the FSR 2 permutations route and the missing workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 56; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent (no native Linux Vulkan device); `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. A Windows acceptance batch would exit 3 (incomplete) while the four workloads are unimplemented, so it was not started.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 57; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries (lines 88, 108, 122, 132); no `acceptance` run in `artifacts/bench`.
- No source change, build, or bench run. Blockers unchanged: native Linux Vulkan GPU report, Windows/DX12 and Windows/Vulkan 3x180 s acceptance runs on the RTX 5080, the FSR 2 permutations route (user decision), and the physics/streaming/all_features workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 58; no code change; no COMPLETE signal)
- Re-verified: `/dev/dri` absent; `third_party/fsr2` has 0 `*_permutations.h`; `benchmarks/manifest.json` has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`; `samples/` absent. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Acceptance needs Windows/DX12, Windows/Vulkan, and native Linux/Vulkan 3x180 s reports on the reference machine, plus the FSR 2 permutations route and the missing workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 59; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; FSR 2 permutations 0; manifest has 4 implementation:none entries; no acceptance run in artifacts/bench; samples/ absent; RTX 5080 visible to WSL nvidia-smi only.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 60; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; FSR 2 permutations 0; manifest has 4 implementation:none entries (lines 88, 108, 122, 132); no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 61; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); third_party/fsr2 has 0 *_permutations.h; benchmarks/manifest.json has 4 implementation:none entries; no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run. Blockers unchanged: native Linux Vulkan GPU report, Windows/DX12 and Windows/Vulkan 3x180 s acceptance runs, the FSR 2 permutations route (user decision), the unimplemented physics/streaming/all_features workloads, and the open frame budget (p99 over 16.7 ms on render_stress).
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 62; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; manifest has 4 implementation:none entries (lines 88, 108, 122, 132); no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No source change, build, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Blockers need user decisions or the reference machine (see earlier entries). No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 63; Linux quality gates run; no COMPLETE signal)
- Re-verified blockers: `/dev/dri` absent; FSR 2 permutations 0; manifest has 4 `implementation: none` entries; no `acceptance` run in `artifacts/bench`.
- Linux gates passed: `cmake -P cmake/ValidateVendor.cmake` (26 dependencies, 8542 files); `cmake --preset linux-dev`; `cmake --build --preset linux-debug`; `ctest --preset linux-unit --no-tests=error` (30/30 passed, 1 unit + 29 integration).
- Not run this pass: Windows gates (need the cmd.exe/.bat route), Linux GCC/ASan presets, and all GPU/benchmark acceptance runs (blocked, see above).
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 64; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; manifest has 4 implementation:none entries (physics_action_sequence_v1, streaming_route_v1, and two more); acceptance runs only us076-*, us077-*, us078-* in artifacts/bench. No source file newer than this log.
- No build, gate, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the user's decisions (FSR 2 vendoring route, frame-budget contract question) and the RTX 5080 reference machine for native Linux Vulkan. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 65; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; manifest has 4 implementation:none entries; no acceptance run in artifacts/bench; samples/ absent.
- No build, gate, or bench run: the acceptance batch needs the RTX 5080 Windows and native Linux reports and the four unimplemented workloads.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 66; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; manifest has 4 implementation:none entries; no acceptance run in artifacts/bench.
- No build, gate, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine (Windows DX12/Vulkan, native Linux Vulkan), the FSR 2 vendoring decision, and the four workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 67; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; manifest has 4 implementation:none entries (lines 88, 108, 122, 132); no acceptance-suite run in artifacts/bench (only us076/us077/us078/us080 partial runs). nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 route and frame-budget question need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 68; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132) and one `builtin`; no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL; cmd.exe is reachable for Windows runs.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 vendoring route and any contract change need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 69; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); no acceptance run in artifacts/bench (only us076/us077/us078 partial runs); samples/ absent.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 vendoring route and any contract change need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 70; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries; no acceptance run in artifacts/bench; samples/ absent.
- No build, gate, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 route, the all_features route, and the frame-budget question need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 71; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries; no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 route, the all_features route, and the frame-budget contract question need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 72; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries; no acceptance run in artifacts/bench (only us076/us077/us078/us080 partial runs). nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented, and the FSR 2 route, the all_features route, and the frame-budget contract question need the user.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 73; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); no acceptance run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine for native Linux Vulkan, the FSR 2 vendoring decision, and the four workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 74; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); artifacts/bench has no acceptance-suite run (us076/us077/us078/us080 partial only). nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine for native Linux Vulkan, the FSR 2 vendoring decision, the four workloads, and the frame-budget question if a budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 75; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); no `acceptance` run in artifacts/bench; no source file newer than this log.
- No build, gate, or bench run: the acceptance batch needs the RTX 5080 reference machine (Windows DX12/Vulkan, native Linux Vulkan) and the four unimplemented workloads; a run here would exit 3 (incomplete).
- Stop: US-080 cannot be signed off on this host. Needs the user's decisions (FSR 2 vendoring route, frame-budget contract question). No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 76; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 FSR 2 *_permutations.h; manifest has implementation "none" for physics_action_sequence_v1 (line 119 route), streaming_route_v1 (line 129 route), and two more (lines 88, 108, 122, 132 entries).
- No build or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine (native Linux Vulkan), the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 77; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); artifacts/bench has only partial us076/us077/us078/us080 runs, no acceptance suite. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine for native Linux Vulkan, the FSR 2 vendoring decision, the four workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 78; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 implementation "none" entries; no acceptance-suite run in artifacts/bench.
- No build or bench run: a 3x180 s acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 79; Linux gates re-run; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 FSR 2 *_permutations.h; benchmarks/manifest.json has 4 `implementation: "none"` entries; no acceptance-suite run in artifacts/bench.
- Linux quality gates pass on this host: `cmake -P cmake/ValidateVendor.cmake` (26 deps, 8542 files), `cmake --preset linux-dev`, `cmake --build --preset linux-debug`, `ctest --preset linux-unit` (integration 29 tests, unit 1 test, all pass; logs in /tmp/us080_*.log).
- Not run: Windows gates (need cmd.exe batch from WSL), linux-gcc, linux-asan, all GPU suites, and the acceptance bench (a 3x180 s batch would exit 3 while four workloads are unimplemented).
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine (Windows DX12/Vulkan, native Linux Vulkan), the FSR 2 vendoring decision, the four workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 80; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` in the tree; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132); no acceptance-suite run in artifacts/bench.
- No build, gate, or bench run. The acceptance batch needs the RTX 5080 reference machine for native Linux Vulkan and the unimplemented workloads; a run here would exit 3 (incomplete).
- Stop: US-080 cannot be signed off on this host. Needs the user's decisions (FSR 2 vendoring route, unimplemented workloads, frame-budget question if exceeded). No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 81; Windows gates re-run; no COMPLETE signal)
- Windows quality gates pass on the current tree (batch `us080_gate.bat`, logs in Temp `us080_*.log`): `cmake -P cmake/ValidateVendor.cmake` (vendor=0), `cmake --preset win-dev` (configure=0), `cmake --build --preset win-debug` (build=0, 0 error lines), `ctest --preset win-unit` (unit=0; integration 28 tests, unit 1 test).
- Performance acceptance still not run: `all_features` has no source (manifest `implementation: "none"`); `render_stress`, `physics_stress`, and `streaming_stress` have source but the manifest still marks them `none`, and `main.cpp` refuses the physics/streaming visual paths; `/dev/dri` absent (no native Linux Vulkan); 0 FSR 2 `*_permutations.h`.
- Stop: no COMPLETE signal. Remaining: wire `all_features`, decide the FSR 2 route, run Linux gates on this host again only if the tree changes, and run the 3x180 s acceptance batches on the RTX 5080 (Windows DX12/Vulkan and native Linux Vulkan).
---

## 2026-10-27 - US-080 (status check 82; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 `*_permutations.h`; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132); no `all_features` source; no acceptance-suite run in artifacts/bench. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine for native Linux Vulkan, the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 83; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132); artifacts/bench holds only partial us076/us077/us078/us080 runs, no acceptance suite. nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 reference machine for native Linux Vulkan, the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 84; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 FSR 2 `*_permutations.h` under third_party; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132).
- No build, gate, or bench run: the acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 cannot be signed off on this host. Needs the RTX 5080 native Linux Vulkan run, the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 85; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132); nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: the acceptance batch needs native Linux Vulkan hardware and the unimplemented workloads, and would exit 3.
- Stop: US-080 is not signed off. Needs the RTX 5080 native Linux Vulkan run, the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 86; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132).
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 is not signed off. Needs the RTX 5080 native Linux Vulkan run, the FSR 2 vendoring decision, the unimplemented workloads, and a user decision if a frame budget is exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 87: measured cause of the frame-budget miss; no code change; no COMPLETE signal)
- Read `artifacts/bench/us076-dx12-meshlets/report.json` (native_1080, DX12 meshlets, repeat 1; code unchanged since that run). CPU frame time: p50 29.7 ms, p99 42.4 ms, max 59.7 ms. GPU frame time: p50 3.3 ms, p99 11.5 ms, max 24.1 ms. The miss is on the CPU side; the GPU fits 16.7 ms at p99 with margin.
- Budget checks on that run: p99 42.4 ms vs 16.7 ms failed; 0% of frames within 16.7 ms (threshold 99%) failed; 21% of frames over 33.3 ms (limit 0.1%) failed; no frame over 100 ms passed. Residency is not measured (`null`), so the run is marked incomplete.
- Next concrete step (unblocked, does not need the user): profile the CPU frame of `render_stress` (extraction, culling, light and renderable submission for 20000 renderables and 129 lights) on the RTX 5080 via cmd.exe, and fix the measured hotspot. A contract change is not an option.
- Still blocked on the user and the hardware: native Linux Vulkan (no /dev/dri), FSR 2 permutations, and the unimplemented workloads. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 88; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has four `implementation: "none"` entries (lines 88, 108, 122, 132); no `all_features` source in tools/benchmark/src; nvidia-smi sees the RTX 5080 (driver 617.42) under WSL only.
- No build, gate, or bench run: an acceptance batch would exit 3 (incomplete) while those workloads are unimplemented.
- Stop: US-080 is not signed off. Open items: the render_stress CPU-frame hotspot (us076 meshlets run: CPU p99 42.4 ms vs 16.7 ms budget) needs profiling on the RTX 5080 via cmd.exe; native Linux Vulkan needs a host with /dev/dri; FSR 2 needs a vendoring decision; the four unimplemented workloads need source. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 89: CPU hotspot read, no profile; no code change; no COMPLETE signal)
- Read the render_stress frame path. `extract_frame` (engine/render/src/frame_snapshot.cpp) is a per-renderable sphere test with no allocation beyond the output vectors, so it is unlikely to hold a 30 ms CPU frame for 20000 renderables. The remaining per-frame cost is in `SceneRenderer::render` (engine/render/src/scene_renderer.cpp, 3883 lines), which the read did not settle.
- Not done: the hotspot needs a Release profile on the RTX 5080 via cmd.exe (build, then timed sections around extract, render, and fence waits). Not run in this turn.
- Still blocked on the user and hardware: native Linux Vulkan (no /dev/dri), FSR 2 permutations, the four `implementation: "none"` workloads, and the frame-budget contract question. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 90; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; manifest has 4 `implementation: "none"` entries; 0 FSR 2 `*_permutations.h`; no acceptance-suite run in artifacts/bench.
- No build or bench run. Open items unchanged: render_stress CPU-frame hotspot (us076 meshlets p99 42.4 ms vs 16.7 ms) needs a Release profile on the RTX 5080 via cmd.exe; native Linux Vulkan needs a host with /dev/dri; FSR 2 vendoring decision; four unimplemented workloads; residency accessor not implemented.
- Stop: US-080 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 91; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); manifest has 4 `implementation: "none"` entries; 0 FSR 2 `*_permutations.h` under third_party; no acceptance-suite run in artifacts/bench; nvidia-smi sees the RTX 5080 under WSL only.
- No build or bench run. US-080 is not signed off. Open items unchanged: render_stress CPU-frame hotspot (us076 meshlets p99 42.4 ms vs 16.7 ms), native Linux Vulkan host, FSR 2 vendoring decision, four unimplemented workloads, residency accessor, and a user decision if a budget stays exceeded.
---

## 2026-10-27 - US-080 (status check 92; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; manifest has 4 `implementation: "none"` entries; 0 FSR 2 `*_permutations.h` under third_party; no acceptance-suite run in artifacts/bench.
- No build or bench run. US-080 is not signed off; open items as in status check 91. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 93; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; manifest still has four `implementation: "none"` entries (lines 88, 108, 122, 132); no acceptance-suite run in artifacts/bench.
- No build or bench run: the acceptance batch needs native Linux Vulkan hardware and the unimplemented workloads, and would exit 3.
- Stop: US-080 is not signed off. Repeated status checks (about 90 since the first blocked entry) have not changed these facts, so further unattended iterations add nothing. Needs the user: a native Linux Vulkan host, the FSR 2 vendoring route, the `all_features`/physics/streaming workload sources, the render_stress CPU hotspot profile on the RTX 5080 (us076 meshlets CPU p99 42.4 ms vs 16.7 ms), and a decision if a budget stays exceeded. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 94; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; manifest still has four `implementation: "none"` entries (lines 88, 108, 122, 132); RTX 5080 visible under WSL only.
- No build or bench run. US-080 is not signed off. Open items as in status check 93. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 95; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 4 `implementation: "none"` entries in benchmarks/manifest.json; 0 `*_permutations.h` under third_party; no `all_features` source; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run; the blockers are unchanged since status check 94.
- Stop: US-080 is not signed off. Repeated unattended checks add nothing; this loop should not be rescheduled until the user decides the open items (native Linux Vulkan host, FSR 2 vendoring route, the four workload sources, and the render_stress CPU hotspot profile or a frame-budget decision). No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 96; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has 4 `implementation: "none"` entries; no acceptance-suite run in artifacts/bench; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run: an acceptance batch would exit 3 (incomplete) while those blockers stand.
- Stop: US-080 is not signed off. Open items as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 97; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; manifest still has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); no acceptance-suite run in artifacts/bench; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. Blockers unchanged since status check 95; see that entry for the open-items list. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 98; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. US-080 is not signed off. Blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 99; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; benchmarks/manifest.json still has 4 `implementation: "none"` entries (lines 88, 108, 122, 132); RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. US-080 is not signed off. Blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 100; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; no acceptance-suite run in artifacts/bench; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. Blockers as in status check 95. US-080 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 101; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json. artifacts/bench has us080-dx12-readback-off and us080-dx12-meshlets-readback-off (not reviewed this turn); no acceptance-suite run.
- No build or bench run. US-080 is not signed off. Blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 102; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json; RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. US-080 is not signed off; blockers as in status check 95. Repeated unattended checks add nothing; the user must decide the open items. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 103; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json (lines 88, 108, 122, 132); RTX 5080 (driver 617.42) visible under WSL only; no acceptance-suite run in artifacts/bench.
- No build or bench run. US-080 is not signed off; blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 104; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json; latest artifacts are us080 readback-off runs only, no acceptance-suite run.
- No build or bench run. US-080 is not signed off. Blockers as in status check 95 (native Linux Vulkan host, FSR 2 vendoring route, four workload sources, render_stress CPU hotspot or a frame-budget decision). No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 105; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent (no native Linux Vulkan device); 4 `implementation: "none"` entries in benchmarks/manifest.json; 0 FSR 2 `*_permutations.h` under third_party; no acceptance-suite run in artifacts/bench (only two us080 readback-off runs); RTX 5080 (driver 617.42) visible under WSL only.
- No build or bench run. US-080 is not signed off; blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-080 (status check 106; no code change; no COMPLETE signal)
- Re-verified: /dev/dri absent; 0 `*_permutations.h` under third_party; 4 `implementation: "none"` entries in benchmarks/manifest.json; no source, workload, or bench file newer than the us080 readback-off runs (2026-10-26 23:59).
- No build or bench run. US-080 is not signed off; blockers as in status check 95. No COMPLETE signal.
---

## 2026-10-27 - US-081 (documentation and gate run; not signed off)
- Added the root `README.md` (OS/SDK requirements, offline CMake setup, editor workflow, Lua API, asset pipeline, native packaging, benchmarks, tests, licensing).
- Added `docs/architecture.md` (layers and link rules from the CMake graph), `docs/schemas.md` (scene, prefab, component, sidecar, pack, save, package, manifest), `docs/vendor.md` (manifest, licenses, notices, FSR 2 gap), `docs/capability-matrix.md`, `docs/acceptance.md` (story-to-test map for US-001..US-080, gate results, open items).
- Benchmark reproduction already lives in `docs/benchmarks.md` (Reproduction section); linked from the README rather than duplicated.
- Gates run here: Linux `ValidateVendor` passed (26 deps, 8542 files); `linux-dev` configure, `linux-debug` build, and `linux-unit` (30/30) passed. Windows `ValidateVendor`, `win-dev` configure, `win-debug` build, and `win-unit` (29/29) passed.
- Not run: `linux-gcc`, `linux-asan`/`linux-sanitize`, `win-release`/`linux-release` builds, GPU suites (DX12, Vulkan on Windows; Vulkan on native Linux), editor suite and visual smoke tests, the offline clean-copy build, clean-directory package smoke tests, and the full acceptance benchmark. Final acceptance stays incomplete.
- No source code changed; the docs describe the tree as it is.
- **Learnings:**
  - The first US-081 Windows batch reported `build=0`, but its build log stopped at step 33/171 and the ctest ran while the log was incomplete. The cause was not determined. The batch was rerun with the build alone: `build=0`, log complete, and ctest `unit=0`. Rerun a build on its own, and check its log, before counting a ctest that follows it.
  - Deleting `us081_*.txt` between batches also removed the validate and configure exit codes of the first run. Their logs were kept, and the rerun batch covered the build and ctest steps.
  - The README must not call a platform verified until its gate result is recorded in `docs/acceptance.md`.
---

## 2026-10-27 - US-081 (re-verification; not signed off; no COMPLETE signal)
- Re-ran the Linux feasible gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` passed; `cmake --preset linux-dev` passed; `cmake --build --preset linux-debug` passed; `ctest --preset linux-unit` passed 30 of 30.
- No source or doc changes this pass. The docs written earlier link to existing files (checked the README `docs/` links).
- Still not run, recorded as open in `docs/acceptance.md`: linux-gcc (local GCC 9.4 cannot build Jolt), linux-asan/linux-sanitize, win-release and linux-release builds, all GPU suites (DX12 and Vulkan on Windows, Vulkan on native Linux), editor suite and visual smoke tests, offline clean-copy build, clean-directory package smoke tests, and the full acceptance benchmark.
- Stop: final acceptance needs the RTX 5080 reports and native Linux Vulkan hardware, which this machine lacks. No COMPLETE signal.
---

## 2026-10-27 - US-081 (linux-release gate; not signed off; no COMPLETE signal)
- `cmake --build --preset linux-release` first failed: `tests/src/core_assert_tests.cpp` used `ENGINE_DEBUG_ASSERT` inside `CHECK_NOTHROW` (the macro is a statement under NDEBUG), and `failing_debug_assert` left an unused variable. Fixed: the helper is compiled only without NDEBUG, and the release branch calls the macro directly and checks that it returns.
- After the fix: linux-release build passed; `ctest --preset linux-release-unit` passed 30 of 30. Recorded in `docs/acceptance.md`.
- Still not run: win-release, linux-gcc, linux-asan/linux-sanitize, GPU suites, editor suite and visual smoke tests, clean-copy offline build, clean-directory package smoke tests, full acceptance benchmark.
- **Learnings:**
  - Release-only test code is not covered by the debug gates. A `-Werror` release build found a test that expanded to a statement inside a CHECK macro. Build release before counting it verified.
---

## 2026-10-27 - US-081 (status pass; not signed off; no COMPLETE signal)
- Re-ran `cmake -P cmake/ValidateVendor.cmake`: passed (26 dependencies, 8542 files). No code or doc changes this pass.
- Current state is the one recorded in `docs/acceptance.md`: Linux dev/debug/unit/release and Windows validate/dev/debug/unit gates passed in earlier passes.
- Still open, so final acceptance stays incomplete: GPU reports (DX12 and Vulkan on the RTX 5080, native Linux Vulkan), full acceptance benchmark, editor smoke tests with screenshots, linux-gcc (needs GCC 11+), linux-asan/linux-sanitize, win-release, clean-directory package and offline-copy checks, and the license file.
- **Learnings:**
  - When every feasible gate is green and the remaining criteria need hardware, record the gap in `docs/acceptance.md` and stop without COMPLETE. A green gate list does not meet the story while its hardware items are open.
---

## 2026-10-27 - US-081 (status check; no code change; no COMPLETE signal)
- Re-verified: README has the required sections (OS/SDK, offline CMake, editor workflow, Lua API, asset pipeline, native packaging, benchmarks, tests, licensing); `/dev/dri` absent (no native Linux Vulkan device); 4 `implementation: "none"` entries in benchmarks/manifest.json; no acceptance-suite run in artifacts/bench (only us080 readback-off runs).
- No build or gate run: the Linux gates already passed on this tree earlier today (see the entries above), and no source changed since.
- Stop: US-081 is not signed off. Open items as in `docs/acceptance.md`: RTX 5080 GPU reports (DX12, Vulkan on Windows; Vulkan on native Linux), full acceptance benchmark, editor smoke tests with screenshots, linux-gcc (needs GCC 11+), linux-asan/linux-sanitize, win-release, clean-directory package and offline-copy checks. No COMPLETE signal.
---

## 2026-10-27 - US-081 (linux-asan gate; not signed off; no COMPLETE signal)
- `cmake --preset linux-asan`, `cmake --build --preset linux-asan` (exit 0), and `ctest --preset linux-sanitize` passed 30 of 30 with AddressSanitizer and UBSan on the CPU tests. Recorded in `docs/acceptance.md`.
- Re-ran `cmake -P cmake/ValidateVendor.cmake`: passed (26 dependencies, 8542 files).
- Still open (hardware or decisions): RTX 5080 GPU suites (DX12, Vulkan on Windows; Vulkan on native Linux), acceptance benchmark, editor smoke tests with screenshots, linux-gcc (needs GCC 11+), win-release, clean-copy offline build, clean-directory package smoke tests, and a LICENSE file (the project's license is a user decision; none exists in the repo).
- **Learnings:**
  - The sanitizer gate is local with clang 10; it does not need GPU hardware, so run it before calling the Linux CPU gates complete.
---

## 2026-10-27 - US-081 (gate re-run; not signed off; no COMPLETE signal)
- Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0; `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` exit 0.
- No source or doc changes. Open items are unchanged and listed in `docs/acceptance.md` (RTX 5080 GPU reports, acceptance benchmark, editor smoke tests, linux-gcc, win-release, clean-copy and package checks, LICENSE decision). No `/dev/dri` device on this machine.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Read `docs/acceptance.md`: the gate table matches the earlier passes. No source or doc changes this pass, and no gate was re-run.
- `/dev/dri` is absent, so no native Linux Vulkan device. The RTX 5080 reports, acceptance benchmark, and editor smoke tests are still open.
- Feasible but not yet run: `win-release` (via cmd.exe from WSL), the offline clean-copy build, and the clean-directory package smoke tests.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (win-release, offline clean copy; not signed off; no COMPLETE signal)
- `cmake --preset win-dev` and `cmake --build --preset win-release` ran through cmd.exe (batch in Temp): configure 0, release build 0, 596 of 596 steps. Log in `build/us081_win_release.log`.
- Offline clean-copy build: copied the tree without `build/`, `artifacts/`, `.ralph-tui/` to `/tmp/us081_clean`, ran `tools/ci/offline_verify.sh` (network namespace, only `lo`). Exit 0; linux-unit 30 of 30.
- Package smoke: not run as a separate smoke. The distribution integration test covers launch from a clean folder and passed in the unit run. A real sample-project package per OS is still open.
- Updated `docs/acceptance.md` gate table and open items to match.
- Still open (hardware or decisions): RTX 5080 GPU suites (DX12, Vulkan on Windows), native Linux Vulkan GPU, full acceptance benchmark, editor visual smoke tests, linux-gcc (GCC 11+), FSR 2 permutations, LICENSE file.
- **Learnings:**
  - `offline_verify.sh` deletes and rebuilds `build/<preset>` in whatever tree it runs in. Run it in a copy without `build/` to meet the clean-copy requirement, and do not run it on the working tree by accident.
---

## 2026-10-27 - US-081 (Linux editor suite and package check; not signed off; no COMPLETE signal)
- `engine_tests --suite editor` on `build/linux-dev/tests/Debug/engine_tests`: all passed, 106 test cases and 1569 assertions. This suite includes `editor_distribution_tests.cpp`, which builds a package from the sample project into an empty folder and launches it. This is the Linux clean-directory package check; the Windows one is still open.
- Updated the "Standalone packages" row in `docs/acceptance.md` to name this run. No source changes.
- Still open (hardware or decisions): RTX 5080 GPU suites (DX12, Vulkan on Windows; Vulkan on native Linux), full acceptance benchmark, editor visual smoke tests with screenshots, linux-gcc (needs GCC 11+; only GCC 9 installed), a real sample-game package on each OS, and the LICENSE decision.
- **Learnings:**
  - The packaging check is a CPU test: `build_game` launches the staged package from a clean copy, so the editor suite covers the clean-directory launch without a GPU.
---

## 2026-10-27 - US-081 (gate re-run; not signed off; no COMPLETE signal)
- Re-ran on the current tree: `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --build --preset linux-debug` exit 0; `ctest --preset linux-unit --no-tests=error` 30 of 30 passed.
- No source or doc changes. `/dev/dri` is absent, so the native Linux Vulkan GPU report and the RTX 5080 GPU/editor/benchmark reports cannot be produced here. The LICENSE decision is still open. Open items are unchanged in `docs/acceptance.md`.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Read `docs/acceptance.md`: the open items match the gate table. No source or doc changes this pass, and no gate was re-run (the Linux and win-release results above are current).
- `/dev/dri` is absent and no LICENSE file exists, so the native Linux Vulkan GPU report, the RTX 5080 DX12/Vulkan GPU and editor reports, the acceptance benchmark, the visual editor smoke tests, linux-gcc (GCC 11+), and the real sample-game package on each OS remain open.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Linux unit gate re-run; not signed off; no COMPLETE signal)
- `ctest --preset linux-unit --output-on-failure --no-tests=error`: 30 of 30 passed (29 integration, 1 unit), exit 0.
- Earlier this pass: `cmake -P cmake/ValidateVendor.cmake` exit 0, `cmake --build --preset linux-debug` exit 0. No source or doc changes.
- Still open (unchanged): Windows gates and RTX 5080 GPU/editor/benchmark reports, native Linux Vulkan GPU (no /dev/dri), linux-gcc (GCC 11+), visual smoke tests, real sample-game packages per OS, LICENSE decision.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Read `docs/acceptance.md` open items against the machine: `/dev/dri` still absent, no GCC 11+ compiler, no LICENSE file. No source or doc changes, no gate re-run (the Linux and win-release results above are current).
- Still open, unchanged: RTX 5080 GPU suites (DX12, Vulkan on Windows), native Linux Vulkan GPU, full acceptance benchmark, FSR 2 permutations, linux-gcc, visual editor smoke tests, real sample-game packages per OS, LICENSE decision.
- **Learnings:**
  - Repeated status passes add no evidence. Check the open list against the machine once, then stop until hardware, a compiler, or the license decision changes.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- README has every required section (OS/SDK, offline CMake, editor, Lua API, asset pipeline, packaging, benchmarks, tests, licensing). `docs/acceptance.md` maps all stories and lists the open items.
- `/dev/dri` absent, no GCC 11+, no LICENSE file: the same blockers as the previous passes. No source or doc changes, no gate re-run.
- Still open: RTX 5080 DX12/Vulkan GPU and editor suites, native Linux Vulkan GPU, full acceptance benchmark, FSR 2 permutations, linux-gcc, visual editor smoke tests, Windows and real sample-game package smoke tests, LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Windows and Linux release gates; not signed off; no COMPLETE signal)
- Windows gates run from WSL through `cmd.exe` (batch in Temp, one at a time): `cmake -P cmake/ValidateVendor.cmake` exit 0 (26 dependencies, 8542 files); `cmake --preset win-dev` exit 0; `cmake --build --preset win-debug` exit 0 (36 of 36 steps, `engine_tests.exe` linked); `ctest --preset win-unit` 29 of 29 passed.
- Windows GPU on the RTX 5080: `engine_tests --suite gpu --backend dx12` passed (72 cases, validation on); `--backend vulkan` passed (73 cases, `software=no`, validation on, no VUID lines, pinned 1.4.363 layer via `VK_LAYER_PATH`). Windows editor suite on DX12 passed (107 cases).
- `cmake --build --preset win-release` exit 0. The log was 10 lines because the Release tree was already up to date (the external DXC step always prints). `tests/Release/engine_tests.exe` (01:51) is newer than every engine and test source.
- `cmake --build --preset linux-release` exit 0, and `ctest --preset linux-release-unit` 30 of 30 passed.
- Logs saved to `artifacts/reports/us081/`. `docs/acceptance.md` rows updated for the Windows GPU and editor results.
- **Learnings:**
  - A release build log of about 10 lines is an up-to-date tree, not a truncated build. Check the binary's mtime against sources before treating it as built. `ninja -n -f build-Release.ninja all` shows only the external step when nothing is dirty.
  - Open items are unchanged: native Linux Vulkan GPU (no `/dev/dri`), linux-gcc (GCC 9.4 here), full acceptance benchmark, FSR 2 permutations, visual editor smoke tests on both OSes, a standalone package from a real sample game on each OS, Windows Vulkan editor suite, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- No source, test, or doc file is newer than `artifacts/reports/us081/`, so the gate results recorded above still describe the current tree. No gate re-run and no doc changes this pass.
- Blockers, checked against the machine: no `/dev/dri` (native Linux Vulkan GPU), only GCC 9.4 (linux-gcc needs GCC 11+), no LICENSE file. Still open: visual editor smoke tests on both OSes, Windows Vulkan editor suite, full acceptance benchmark, FSR 2 permutations, and a package built from a real sample game on each OS.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (blocker check; not signed off; no COMPLETE signal)
- Checked the blockers against the machine: `/dev/dri` absent, only GCC 9.4 installed, no LICENSE file, and no FSR 2 `*_permutations.h` under `third_party`. None has changed.
- `docs/acceptance.md` and `artifacts/reports/us081/` were written in the same pass (02:20), so the recorded gate results match the tree. No source, test, or doc change this pass, and no gate re-run.
- Still open, as in `docs/acceptance.md`: RTX 5080 Vulkan GPU report on native Linux, full acceptance benchmark, visual editor smoke tests on both OSes, linux-gcc (GCC 11+), the Windows Vulkan editor suite, real sample-game packages per OS, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Checked the tree against `artifacts/reports/us081/`: only `docs/acceptance.md` is newer than the gate logs (a status edit). No source, test, or build change, and no gate re-run.
- Blockers, checked on the machine: `/dev/dri` absent (native Linux Vulkan GPU), GCC 9.4 only (linux-gcc needs GCC 11+), no LICENSE file, no FSR 2 `*_permutations.h`.
- Still open, as in `docs/acceptance.md`: RTX 5080 full acceptance benchmark, visual editor smoke tests on both OSes, Windows Vulkan editor suite, real sample-game packages per OS, LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Re-checked the tree against `artifacts/reports/us081/`: only `docs/acceptance.md` and `.ralph-tui` bookkeeping changed since the gate logs. `third_party/dxc/utils/GetCommitInfo.py` has a newer mtime; not an engine change, and the vendor check still passes.
- `cmake -P cmake/ValidateVendor.cmake` passes on Linux (26 dependencies, 8542 files). No gate re-run beyond that.
- Blockers, checked on the machine: `/dev/dri` absent (native Linux Vulkan GPU), GCC 9.4 only (linux-gcc needs GCC 11+), no LICENSE file, 0 FSR 2 `*_permutations.h` files.
- Still open, as in `docs/acceptance.md`: RTX 5080 full acceptance benchmark, visual editor smoke tests on both OSes, Windows Vulkan editor suite, real sample-game packages per OS, LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Windows Vulkan editor suite; not signed off; no COMPLETE signal)
- `engine_tests --suite editor --backend vulkan` on Windows (RTX 5080 host, pinned 1.4.363 layer via `VK_LAYER_PATH`): passed, 107 test cases and 1587 assertions. No VUID or error lines in the log. Saved to `artifacts/reports/us081/win-editor-vulkan.log`.
- Updated the editor row in `docs/acceptance.md`. No source changes; no other gate re-run.
- Still open (hardware or decisions): native Linux Vulkan GPU (no `/dev/dri`), full acceptance benchmark, visual editor smoke tests with screenshots on both OSes, linux-gcc (GCC 9 only), FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Windows editor launch screenshot; not signed off; no COMPLETE signal)
- Launched `build/win-dev/tools/game_editor/Release/game_editor.exe` from WSL through PowerShell, waited 8 s, captured the primary screen. The editor was alive and showed all panels, with the console at "no project is open". The process was stopped afterwards. Screenshot: `artifacts/reports/us081/win-editor-smoke-launch.png`.
- Updated the visual smoke row in `docs/acceptance.md` to say launch only. No project was opened and no action sequence ran, so the full smoke test is still open.
- Still open: RTX 5080 native Linux Vulkan GPU, full acceptance benchmark, Linux visual smoke, linux-gcc (GCC 9 only), FSR 2 permutations, Windows clean-directory package from a real sample game, LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (package-test coverage on Windows; not signed off; no COMPLETE signal)
- The Windows editor logs (`win-editor-dx12.log`, `win-editor-vulkan.log`) pass 107 cases each. `tests/src/editor_distribution_tests.cpp` is tagged `[editor][integration]`, so the clean-folder package case runs in that suite. The log has no per-case names, so this is inferred from the tag.
- `docs/acceptance.md` updated: the standalone-packages row and open item 6 now record the Windows and Linux editor-suite results. No source or build change; no gate re-run.
- Still open, as in `docs/acceptance.md`: native Linux Vulkan GPU (no `/dev/dri`), full acceptance benchmark, visual smoke beyond launch on Windows, Linux visual smoke, linux-gcc (GCC 9.4 only), FSR 2 permutations, a package from a real sample game, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Checked the required docs (architecture, schemas, vendor, benchmarks, capability matrix, acceptance, build) and the README sections. All exist. `cmake -P cmake/ValidateVendor.cmake` passes on Linux (26 dependencies, 8542 files).
- No source, test, or doc file changed since `docs/acceptance.md`; only `tasks/prd.json` is newer. No gate re-run.
- Blockers, unchanged: no `/dev/dri` (native Linux Vulkan GPU), GCC 9.4 only (linux-gcc needs GCC 11+), no LICENSE file, no FSR 2 `*_permutations.h`. Also open: full acceptance benchmark, Linux visual smoke, Windows visual smoke beyond launch, a package from a real sample game on each OS.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `ctest --preset linux-unit --output-on-failure --no-tests=error` re-run on the current tree: 30 of 30 passed (1 unit, 29 integration).
- Linux visual smoke attempted under WSLg (DISPLAY=:0). `build/linux-dev/tools/game_editor/Release/game_editor` exits at start: `cannot create the vulkan device: no hardware Vulkan 1.2 adapter`. No Linux screenshot tool is installed. `docs/acceptance.md` visual smoke row updated with this result.
- Blockers, unchanged: no `/dev/dri` (native Linux Vulkan GPU), GCC 9.4 only (linux-gcc needs GCC 11+), no LICENSE file, FSR 2 `*_permutations.h` absent (0 files under `third_party`).
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (blocker re-check; not signed off; no COMPLETE signal)
- Re-checked the blockers on this machine: `/dev/dri` absent (no native Linux Vulkan GPU), GCC 9.4 only and no `g++-11` (linux-gcc needs GCC 11+), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`.
- `docs/acceptance.md` still lists these as `not run`, which matches the machine. No source, test, or doc change this pass and no gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark (RTX 5080), Linux visual smoke, Windows visual smoke beyond launch, linux-gcc, FSR 2 permutations, a package from a real sample game on each OS, and the LICENSE decision.
- **Learnings:**
  - Before re-running gates on a repeat pass, check the hardware and decision blockers first. They are one-line checks, and they decide whether a gate can run at all.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Re-checked the open items in `docs/acceptance.md` against the machine: `/dev/dri` absent (no native Linux Vulkan device), no LICENSE file (a licensing decision for the user), 0 FSR 2 `*_permutations.h` under `third_party`, and no GCC 11+ compiler (linux-gcc cannot run here). The doc already lists each of these as `not run`, and it matches the machine.
- No source, test, or doc change this pass; no gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision.
- **Learnings:**
  - Remaining work here is blocked by hardware, a compiler install, and user decisions (licence, FSR vendoring route). Re-running gates does not change those; check them first and stop when nothing changed.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Re-checked the blockers on this machine: `/dev/dri` absent, GCC 9.4 only (no GCC 11+), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`. Each matches `docs/acceptance.md`.
- No source, test, or doc change this pass; no gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (gate re-run; not signed off; no COMPLETE signal)
- Re-ran on Linux: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` configured and generated (exit 0).
- Blockers unchanged: `/dev/dri` absent, GCC 9.4 only, no LICENSE file, 0 FSR 2 `*_permutations.h`. Did not re-run the build or ctest this pass.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark (RTX 5080), visual smoke on both OSes beyond launch, linux-gcc, FSR 2 permutations, a package from a real sample game on each OS, LICENSE decision.
- **Learnings:**
  - Configure runs take about two minutes here; start them in the background and watch the output file rather than letting the foreground call time out.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Re-checked blockers: `/dev/dri` absent, GCC 9.4 only (no GCC 11+), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`. Unchanged from `docs/acceptance.md`.
- `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files). No source, test, or doc change; no other gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package from a real sample game on each OS, and the LICENSE decision (needs the user).
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- README has every required section (OS/SDK, offline CMake, editor workflow, Lua API, asset pipeline, native packaging, benchmarks, tests, licensing). The required docs exist under `docs/`.
- Blockers re-checked and unchanged: `/dev/dri` absent (no native Linux Vulkan GPU), no `g++-11` (linux-gcc needs GCC 11+), no LICENSE file (user decision), 0 FSR 2 `*_permutations.h` under `third_party` (user's vendoring choice).
- No source, test, or doc change this pass; no gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Blockers re-checked on this machine: `/dev/dri` absent (no native Linux Vulkan device), GCC 9.4 only (no GCC 11+ or clang 14+ for linux-gcc), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`.
- Files newer than `artifacts/reports/us081/`: only `docs/acceptance.md`, `tasks/prd.json`, and the editor launch screenshot. No source, test, or build change; no gate re-run.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision (needs the user).
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (gate re-run; not signed off; no COMPLETE signal)
- Blockers re-checked: `/dev/dri` absent, GCC 9.4 only (no g++-11 or clang 14+), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`.
- Re-ran `cmake --build --preset linux-debug` (exit 0) and `ctest --preset linux-unit --output-on-failure --no-tests=error` (30 of 30 passed: 1 unit, 29 integration). Matches `docs/acceptance.md`.
- No source or doc change this pass.
- Still open: native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, and the LICENSE decision (needs the user).
- **Learnings:**
  - `sleep` over a few seconds is blocked in this shell. Wait on a background build with a Monitor that greps a BUILD_EXIT marker the wrapper writes.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (gate re-run; not signed off; no COMPLETE signal)
- Re-ran `cmake -P cmake/ValidateVendor.cmake` (passed: 26 dependencies, 8542 files), `cmake --build --preset linux-debug` (exit 0), and `ctest --preset linux-unit --output-on-failure --no-tests=error` (30 of 30 passed).
- Blockers unchanged: `/dev/dri` absent (no native Linux Vulkan device), no GCC 11+ or clang 14+ for linux-gcc, no LICENSE file (user decision), no FSR 2 `*_permutations.h` under `third_party`.
- No source or doc change this pass.
- Still open: Windows gates from this session, native Linux Vulkan GPU suite, full acceptance benchmark on the RTX 5080, visual editor smoke on both OSes, linux-gcc, FSR 2 permutations, a package built from a real sample game on each OS, clean-copy offline build, and the LICENSE decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Confirmed the tree is unchanged since the recorded gate runs: the only files newer than `artifacts/reports/us081/` are `docs/acceptance.md` and `tasks/prd.json`. No source, test, or build change; no gate re-run.
- Blockers, as in `docs/acceptance.md`: no `/dev/dri` (no native Linux Vulkan device), no GCC 11+ (`linux-gcc`), FSR 2 `*_permutations.h` absent, no LICENSE file (user decision), acceptance benchmark not complete (acceptance scenes do not yet produce GPU timestamps), no screenshot-backed visual smoke with an action sequence on either OS.
- **Learnings:**
  - Check `find -newer <gate-artifact-dir>` before any re-run. It answers "did anything change since the last gate" in one command.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` shows no source, test, or build change since the recorded gate runs; only `docs/acceptance.md`, `tasks/prd.json`, and ralph-tui bookkeeping are newer.
- Blockers still present: no `/dev/dri` (no native Linux Vulkan device), no GCC 11+ or clang 14+ (linux-gcc), no LICENSE file (user decision), no FSR 2 `*_permutations.h`, and no RTX 5080 GPU/acceptance benchmark reports.
- No source or doc change this pass; no gate re-run.
- **Learnings:**
  - Repeated status passes add no evidence. Run a gate again only after `find -newer` shows a change.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, a third_party DXC build-touched file, and the editor launch screenshot. No source, test, or build change since the 02:20 Windows gate runs, so no gate was re-run.
- Windows logs in `artifacts/reports/us081/` agree with `docs/acceptance.md`: debug build, unit ctest, DX12 and Vulkan GPU suites on the RTX 5080, and DX12 and Vulkan editor suites all passed.
- Open items, unchanged: native Linux Vulkan GPU report (no `/dev/dri`), full acceptance benchmark (acceptance scenes produce no GPU timestamps yet), FSR 2 permutations (user's vendoring choice), linux-gcc (needs GCC 11+), LICENSE file (user decision), visual editor smoke beyond launch on both OSes, and a package from a real sample game on each OS.
- **Learnings:**
  - The Windows results live in `artifacts/reports/us081/*.log` and are already mapped in `docs/acceptance.md`. Read them before running anything on Windows.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `tasks/prd.json`, `docs/acceptance.md`, and the editor launch screenshot. No source, test, or build change since the recorded gates, so no gate was re-run.
- Blockers re-checked: no `/dev/dri`, GCC 9.4 only (no GCC 11+ for linux-gcc), no LICENSE file, 0 FSR 2 `*_permutations.h` under `third_party`.
- Open items match `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark (acceptance scenes produce no GPU timestamps yet), FSR 2 permutations, linux-gcc, visual editor smoke beyond launch on both OSes, a package built from a real sample game, and the LICENSE decision (needs the user).
- **Learnings:**
  - The story's remaining acceptance items need hardware or a user decision, so repeated passes add no evidence.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and the editor launch screenshot. No source, test, or build change since the recorded gates, so no gate was re-run.
- Blockers re-checked: `/dev/dri` absent, GCC 9.4 only (no GCC 11+ for linux-gcc, no clang 14+), no LICENSE file, FSR 2 permutations absent.
- Open items match `docs/acceptance.md` (native Linux Vulkan GPU, full acceptance benchmark, FSR 2, linux-gcc, visual editor smoke beyond launch, package from a real game, LICENSE).
- **Learnings:**
  - Repeated status passes add no evidence; re-run a gate only after `find -newer` shows a change.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` shows no source, test, build, or CMake change since the recorded gates. Changed files are docs/acceptance.md, tasks/prd.json, ralph-tui bookkeeping, the editor launch screenshot, and a DXC helper script under third_party (GetCommitInfo.py, not built by the engine gates). No gate was re-run.
- Blockers re-checked: `/dev/dri` absent, no LICENSE file (user decision), zero FSR 2 `*_permutations.h` under third_party (user's vendoring choice), GCC 9.4 only for linux-gcc.
- Open items match docs/acceptance.md: native Linux Vulkan GPU report, full acceptance benchmark on the RTX 5080 (acceptance scenes produce no GPU timestamps yet), visual editor smoke beyond launch on both OSes, a package built from a real sample game on each OS, clean-copy offline build, linux-gcc, and the LICENSE decision.
- **Learnings:**
  - The `find -newer` check is the cheapest way to confirm no re-run is needed; the remaining items need hardware or a user decision.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Re-checked blockers: `/dev/dri` absent, no g++-11 or clang++-14 (only GCC 9.4), zero FSR 2 `*_permutations.h` under third_party, no LICENSE or COPYING file.
- The README and docs cover each documentation item (OS/SDK, offline CMake, editor, Lua API, asset pipeline, packaging, architecture, schemas, vendor, benchmarks, capability matrix, acceptance map). No source, test, or build change, so no gate was re-run.
- Open items are the same as `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark on the RTX 5080, visual editor smoke with an action sequence on both OSes, linux-gcc, FSR 2 permutations, a package from a real sample game on each OS, and the LICENSE decision (user).
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, the Windows editor launch screenshot, and a DXC helper script (`third_party/dxc/utils/GetCommitInfo.py`). No source, test, or CMake change since the recorded gates, so no gate was re-run.
- Blockers re-checked: `/dev/dri` absent (no native Linux Vulkan device), GCC 9.4 only (no GCC 11+ for linux-gcc), no LICENSE or COPYING file (user decision), FSR 2 `*_permutations.h` absent (user's vendoring choice).
- Open items match `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark on the RTX 5080 (acceptance scenes produce no GPU timestamps yet), visual editor smoke beyond launch on both OSes, linux-gcc, a package built from a real sample game on each OS, clean-copy offline build, and the LICENSE decision.
- **Learnings:**
  - Repeated status passes add no evidence. Run a gate again only after `find -newer` shows a change.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, the Windows launch screenshot, and `third_party/dxc/utils/GetCommitInfo.py`. No source, test, or CMake change, so no gate was re-run.
- `/dev/dri` is still absent, so there is no native Linux Vulkan device.
- Open items are unchanged and listed in `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark, visual editor smoke beyond launch on both OSes, a package from a real sample game, clean-copy offline build, linux-gcc (needs GCC 11+), FSR 2 permutations (user's vendoring choice), and the LICENSE file (user decision).
- **Learnings:**
  - Re-check with `find -newer` before running any gate. Status passes add no evidence on their own.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, the Windows launch screenshot, and `third_party/dxc/utils/GetCommitInfo.py`. No source, test, or CMake change since the recorded gates, so no gate was re-run.
- `/dev/dri` is still absent, so there is no native Linux Vulkan device.
- Open items are unchanged and listed in `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark on the RTX 5080, visual editor smoke beyond launch on both OSes, linux-gcc (needs GCC 11+), FSR 2 permutations (user's vendoring choice), a package built from a real sample game on each OS, clean-copy offline build, and the LICENSE file (user decision).
- **Learnings:**
  - Run `find -newer artifacts/reports/us081` first. When it shows no source change, a status pass adds nothing and no gate should be re-run.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` shows only docs/acceptance.md, tasks/prd.json, ralph-tui bookkeeping, and the Windows launch screenshot. No source, test, or CMake change, so no gate was re-run.
- Blockers unchanged: `/dev/dri` absent, no LICENSE/COPYING file (user decision), FSR 2 permutations absent, GCC 9.4 only for linux-gcc.
- Open items are in docs/acceptance.md. They need hardware, a native Linux Vulkan device, or a user decision.
- **Learnings:**
  - When the change check shows only docs and bookkeeping, re-running gates adds no evidence.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `tasks/prd.json`, `docs/acceptance.md`, and the Windows launch screenshot. No source, test, or CMake change since the recorded gates, so no gate was re-run.
- `/dev/dri` is still absent (no native Linux Vulkan device). No LICENSE file. GCC 9.4 is the only local GCC (linux-gcc needs 11+). FSR 2 permutations are still absent.
- Open items are unchanged and listed in `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark with GPU timestamps, visual editor smoke beyond launch on both OSes, linux-gcc, a package from a real sample game, and the LICENSE decision (user).
- **Learnings:**
  - Final acceptance needs hardware and a user decision that this environment cannot supply. Record the status and stop; do not signal COMPLETE.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and the Windows launch screenshot. No source, test, or CMake change, so no gate was re-run.
- Open items unchanged (`docs/acceptance.md` "Open items"): native Linux Vulkan GPU report (`/dev/dri` absent), full acceptance benchmark (acceptance scenes produce no GPU timestamps), FSR 2 permutations (user's vendoring choice), linux-gcc (needs GCC 11+), visual editor smoke beyond Windows launch, a package from a real game, and the LICENSE file (user decision).
- **Learnings:**
  - Final acceptance needs hardware, a vendoring choice, and a license decision that this environment cannot supply. Record the status and stop.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists `docs/acceptance.md`, `tasks/prd.json`, the Windows launch screenshot, and `third_party/dxc/utils/GetCommitInfo.py` (mtime only). `cmake -P cmake/ValidateVendor.cmake` passes (26 dependencies, 8542 files), so the vendored file still matches its pin. No first-party source, test, or CMake change, so no gate was re-run.
- README and docs links resolve (checked every `docs/*.md` link target).
- Open items are unchanged and listed in `docs/acceptance.md`: native Linux Vulkan GPU report (`/dev/dri` absent), full acceptance benchmark (render_stress p99 over 16.7 ms; acceptance scenes produce no GPU timestamps), FSR 2 permutations (user's vendoring choice), linux-gcc (needs GCC 11+), native Linux visual smoke, and the LICENSE file (user decision).
- **Learnings:**
  - A vendored file with a new mtime but an unchanged hash is not a source change; `ValidateVendor` confirms it in seconds.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` shows only `docs/acceptance.md`, `tasks/prd.json`, and ralph-tui bookkeeping. No source, test, or CMake change, so no gate was re-run.
- README has the required sections (OS/SDK, offline CMake, editor workflow, Lua API, asset pipeline, native packaging). Architecture, schema, capability-matrix, benchmark, and vendor docs exist.
- Open items are unchanged in `docs/acceptance.md`: native Linux Vulkan GPU report (`/dev/dri` absent), full acceptance benchmark (acceptance scenes produce no GPU timestamps), FSR 2 permutations (user's vendoring choice), linux-gcc (needs GCC 11+), Linux visual smoke, a package from a real game, and the LICENSE file (user decision).
- **Learnings:**
  - The story's own text says unavailable environments leave final acceptance incomplete. With only docs changes since the last gate, the honest status is "not complete", not a repeated COMPLETE.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` shows only `docs/acceptance.md`, `tasks/prd.json`, ralph-tui bookkeeping, and the Windows launch screenshot. No source, test, or CMake change, so no gate was re-run.
- `/dev/dri` is still absent: no native Linux Vulkan device. GCC 9.4 is the only local GCC, so `linux-gcc` cannot run. No LICENSE file (user decision). FSR 2 permutations are still absent (user's vendoring choice).
- Open items are unchanged in `docs/acceptance.md`: native Linux Vulkan GPU report, full acceptance benchmark (no GPU timestamps yet), FSR 2, linux-gcc, visual editor smoke beyond Windows launch, and the LICENSE decision.
- **Learnings:**
  - Repeated status passes add no evidence. Re-run a gate only after `find -newer` shows a source or CMake change.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and the Windows launch screenshot. No source, test, or CMake change since the recorded gates, so no gate was re-run.
- `/dev/dri` is absent (no native Linux Vulkan device). No LICENSE file (user decision). GCC 9.4 is the only local GCC, so `linux-gcc` cannot run. FSR 2 permutations are still absent (user's vendoring choice).
- `docs/acceptance.md` already records the offline clean-copy build and the editor-suite package test as passed. Its open items (native Linux Vulkan GPU report, full acceptance benchmark with GPU timestamps, FSR 2, linux-gcc, Linux visual smoke, a package from a real game, LICENSE) need hardware or a user decision.
- **Learnings:**
  - Five status passes in a row found the same blockers. With no source change, the next useful step is the user's decision on the open items, not another pass.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and ralph-tui bookkeeping. No source, test, or CMake change, so no gate was re-run.
- `/dev/dri` is absent (no native Linux Vulkan device). GCC 9.4 is the only local GCC, so `linux-gcc` cannot run. FSR 2 permutations are missing (user's vendoring choice). No LICENSE file (user decision).
- Open items in `docs/acceptance.md` are unchanged: native Linux Vulkan GPU report, full acceptance benchmark (no GPU timestamps yet), FSR 2, linux-gcc, Linux visual smoke, a package from a real game, and the LICENSE decision.
- **Learnings:**
  - The release gates here need hardware and user decisions this environment cannot supply. Record the status and stop.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and ralph-tui bookkeeping. No source, test, or CMake change, so no gate was re-run.
- `/dev/dri` is absent, so there is no native Linux Vulkan device. GCC 9.4 is the only local GCC, so `linux-gcc` cannot run. FSR 2 permutations are missing (user's vendoring choice). No LICENSE file (user decision).
- Open items in `docs/acceptance.md` are unchanged: native Linux Vulkan GPU report, full acceptance benchmark (no GPU timestamps yet), FSR 2, linux-gcc, Linux visual smoke, a package from a real game, and the LICENSE decision.
- **Learnings:**
  - Status passes with no source change add no evidence. The remaining items need hardware or user decisions.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Linux gates re-run; not signed off; no COMPLETE signal)
- Re-ran the Linux quality gates on the current tree: `cmake -P cmake/ValidateVendor.cmake` passed (26 dependencies, 8542 files); `cmake --preset linux-dev` configured and generated; `cmake --build --preset linux-debug` passed; `ctest --preset linux-unit --output-on-failure --no-tests=error` passed (unit 1 test, integration 29 tests).
- Not run here: `linux-release` build, `linux-gcc` (local GCC 9.4 cannot build Jolt), `linux-asan`/`linux-sanitize`, native Linux Vulkan GPU and editor smoke (`/dev/dri` absent), RTX 5080 benchmark and GPU reports, LICENSE file (user decision), FSR 2 permutations (user's vendoring choice).
- Open items are unchanged in `docs/acceptance.md`. US-081 stays incomplete until the hardware reports and user decisions exist.
- **Learnings:**
  - The Linux gates can be re-verified in WSL in a few minutes when the configure is cached. Running them is cheaper evidence than another status pass.
  - Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, the Windows launch screenshot, and `third_party/dxc/utils/GetCommitInfo.py`. The vendor file changed mtime only: `cmake -P cmake/ValidateVendor.cmake` passed again (26 dependencies, 8542 files). No source or CMake change, so no build or test gate was re-run.
- `/dev/dri` is still absent (no native Linux Vulkan device). Local GCC is 9.4, so `linux-gcc` cannot run. FSR 2 permutations, the LICENSE file, the full acceptance benchmark (GPU timestamps), and the native Linux visual smoke test remain open, as recorded in `docs/acceptance.md`.
- **Learnings:**
  - A vendor file with a new mtime is confirmed by `ValidateVendor` hash checks, not by a rebuild.
- Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Checked `docs/acceptance.md` against the tree. Source, tests, and CMake are unchanged since the last recorded gate run, so no gate was re-run. The docs set (README, architecture, schemas, capability matrix, vendor, benchmarks, distribution) is in place.
- Open items are unchanged: native Linux Vulkan GPU report (no `/dev/dri`), full acceptance benchmark (no GPU timestamps for render/streaming/all-features/physics scenes), FSR 2 permutations (user's vendoring choice), `linux-gcc` (local GCC 9.4 cannot build Jolt), Linux visual smoke test, a package from a real game, and the LICENSE file (user decision; none exists).
- **Learnings:**
  - When the open items are hardware reports and user decisions, a status pass adds no evidence. Record the blockers and stop.
  - Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- `find -newer artifacts/reports/us081` lists only `docs/acceptance.md`, `tasks/prd.json`, and `artifacts/reports/us081/win-editor-smoke-launch.png`. No source, test, or CMake change, so no gate was re-run.
- `/dev/dri` is absent (no native Linux Vulkan device). Local GCC is 9.4, so `linux-gcc` cannot build Jolt. No LICENSE file (user decision). FSR 2 permutations are missing (user's vendoring choice).
- Open items in `docs/acceptance.md` are unchanged: native Linux Vulkan GPU report, full acceptance benchmark (no GPU timestamps yet), FSR 2, linux-gcc, Linux visual smoke beyond the Windows launch, and the LICENSE decision.
- **Learnings:**
  - Each remaining blocker needs hardware, a native OS run, or a user decision. More status passes add no evidence.
  - Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (status check; not signed off; no COMPLETE signal)
- Checked the README sections (OS/SDK, offline CMake, editor, Lua API, asset pipeline, packaging, benchmarks, tests, licensing) and `docs/acceptance.md`. Documentation deliverables are in place. No source, test, or CMake change, so no gate was re-run.
- Open items are unchanged: native Linux Vulkan GPU report (`/dev/dri` absent), full acceptance benchmark with GPU timestamps, FSR 2 permutations (user's vendoring choice), `linux-gcc` (local GCC 9.4 cannot build Jolt), the visual editor smoke test beyond the Windows launch, a package from a real game, and the LICENSE file (none exists; user decision).
- **Learnings:**
  - Status passes add no evidence while the blockers are hardware and user decisions. The next useful step is the user's answer on the open items.
  - Stop: US-081 is not signed off. No COMPLETE signal.
---

## 2026-10-27 - US-081 (Linux gate rerun; not signed off; no COMPLETE signal)
- Reran `cmake --preset linux-dev`, `cmake --build --preset linux-debug`, and `ctest --preset linux-unit --no-tests=error` on the current tree: all exit 0, 30 of 30 tests passed (1 unit, 29 integration). Vendor validation also passed. Log: /tmp/us081/linux-gates.log (not in repo).
- No documentation or source changes. `docs/acceptance.md` Linux rows already match this result.
- Open items are unchanged (see `docs/acceptance.md` Open items): native Linux Vulkan GPU report, full acceptance benchmark, FSR 2 permutations, linux-gcc, visual smoke test, LICENSE.
- **Learnings:**
  - The configure step on the `/mnt/d` tree is slow (9p I/O); allow several minutes before expecting log output.
  - Stop: US-081 is not signed off. No COMPLETE signal.
---
