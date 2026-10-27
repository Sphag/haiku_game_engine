# Sonnet report

Review and hardening pass over the engine that was built with ralph and haiku. Date: 2026-10-27. Host: Windows 11, RTX 5080, MSVC 14.51
(Visual Studio 18), plus Ubuntu 20.04 under WSL2 (clang 10).

Written for: the engine's owner, who wants to know what state it is in and what changed.

## Short version

- The engine is a set of well-tested libraries. Before this pass it was **not a usable product**: the editor had **no components** (an
  entity could only have a transform), Play did **not run any component** (no physics bodies, no scripts), scenes with **prefabs could not be
  opened** in the editor or started in the runtime, the "standalone runtime" **loaded a scene and exited**, and there was **no sample project**.
- The editor's input had real bugs, found by reading the code and by a new test: **modifier keys never reached Dear ImGui** (Ctrl+Z, Ctrl+A,
  Shift and Ctrl clicks were dead), mouse buttons came from Raw Input (the activating click was lost, clicks aimed at a window above the
  editor reached it), and the pointer was read **after** the click.
- All of that is fixed, covered by tests, and a sample project (`samples/showcase`) now uses every feature area and is tested end to end:
  scan, load, simulate, open in the editor, Play, Build Game, launch the packaged game.
- DX12 GPU suite: **72 of 72 cases pass** on the RTX 5080 with validation on. Vulkan GPU suite: **not reproducible from this repository**
  (59 of 73 with the layer installed on this host; details below).
- **Not done / not possible here:** the editor was not driven by real mouse and keyboard input, because the Windows session was **locked**
  during the whole pass. The runtime still **draws nothing**. See "What is still not production ready".

## Git

| Branch | Content |
|---|---|
| `haiku` | One commit, `499331d`: the tree exactly as it was when I started (the `.ralph-tui` logs included; `build/` and `artifacts/` are in `.gitignore`) |
| `sonnet` | Everything below, on top of `haiku` |

The repository did not exist, so I ran `git init` first. `.gitattributes` (`* -text`) was added on `sonnet` so that checkouts never rewrite line
endings; the vendored files are checked by SHA-256 (`cmake -P cmake/ValidateVendor.cmake`) and a CRLF conversion would break that.

## Verification, as measured

| Check | Result |
|---|---|
| Windows Release build (`win-release`) | Passes. First attempt failed to link DXC because I had loaded the VS 2022 environment; the tree was configured with VS 18's 14.51. That is an environment mismatch, not a source bug, but `docs/build.md` says "MSVC 14.38 verified", which is not what the tree is built with |
| `ctest --preset win-release-unit` | **Before: 28 of 29** (`engine_tests_integration` failed). **After: 29 of 29** |
| Windows Debug build, `engine_tests` unit / integration / editor | 1037 / 147 / 110 cases, all pass, no warning-as-error |
| Linux (WSL, clang 10) Debug build, `ctest --preset linux-unit` | Builds with `-Werror`, **30 of 30** pass (the new tests included) |
| `engine_tests --suite gpu --backend dx12` | **72 cases, 43 923 assertions, all pass**, RTX 5080, validation layers on |
| `engine_tests --suite editor --backend dx12` | 111 cases, 1614 assertions, all pass |
| `game_editor --backend dx12 --project samples/showcase --frames 240 --debug-layers` | Opens the sample, runs 240 frames, exit 0, writes the workspace file |
| `engine_tests --suite gpu --backend vulkan` | See the Vulkan section: 59 of 73 |
| Not run | Native Linux Vulkan GPU, `engine_bench --suite acceptance`, Linux GCC, ASan/UBSan (they were passing in the earlier record and the new code was only built with clang Debug), GitHub CI |

### The failing test (Windows)

`Build Game writes a package that launches from a clean folder` and the combined-scene editor test failed with
`'"game_runtime.exe"' is not recognized`. The launch check built `cd /d "<dir>" && "game_runtime.exe" ...`. When
`NoDefaultCurrentDirectoryInExePath` is set (it is on this machine), `cmd` does not look in the current folder for a bare name. The
launcher in the shipped package already used `%~dp0`, so only the check and the test were wrong, but a game that fails its own verification
cannot be packaged. Fixed with `.\`. The tests now also print the build error instead of discarding it.

### Vulkan

The Windows Vulkan GPU record in `docs/acceptance.md` says it ran with a pinned 1.4.363 validation layer through `VK_LAYER_PATH`. That layer
is not built by this tree: `third_party/vulkan_validation_layers` has the CMake and generator scripts but no layer sources. On this host only
the SDK 1.3.224 layer is registered, and it rejects the engine's newer feature structures, so 67 of 73 cases failed at device creation. I made
the debug callback ignore the single message class "unknown VkStructureType" (a layer older than the headers cannot parse the structure; it
is not an engine error). With that, 59 of 73 pass. The remaining 14 fail on VUIDs the old layer produces for mutable descriptor types and mesh
shaders, and four mesh-shader cases also report `visible == 0`, which may be the old layer interfering or a real problem. I cannot tell
from here. **To close this**: build the 1.4.363 layer (or install SDK 1.4.x) and rerun; the repository should either vendor the layer sources or
document the SDK version that is required.

## What I found and fixed

### Editor input and focus

The brief said the editor does not handle inputs and focus properly. I could not confirm visually (the desktop was locked), so this is from
the code and from tests that drive the same entry points.

| # | Defect | Effect | Fix |
|---|---|---|---|
| 1 | The ImGui adapter sent `LeftCtrl` etc. but never `ImGuiMod_Ctrl/Shift/Alt/Super`. Dear ImGui derives `io.KeyCtrl` from the `ImGuiMod_*` keys only | Ctrl+Z / Ctrl+Y never fired, Ctrl+A/C/V in text fields, Ctrl and Shift clicks, modifier-based ImGui features were all dead. Found by a new test | `imgui_input.cpp` sends both, with left/right tracking and a reset on focus loss |
| 2 | Mouse buttons and the wheel came only from Raw Input with a null target, which follows **keyboard focus**, not the window under the cursor | The click that activates the editor window was lost (you had to click twice); a click on a window overlapping the editor reached it; a release outside the window was not tied to the press | `platform_win32.cpp`: buttons and wheel from `WM_*BUTTON*` / `WM_MOUSEWHEEL` with `SetCapture`, and a release on `WM_CAPTURECHANGED`. Raw Input now carries relative motion only |
| 3 | The pointer was polled **after** the frame's button events | A click landed where the cursor was last frame, so fast clicks hit the wrong widget | Pointer is set first, then the buttons |
| 4 | Outside the window the pointer became "none" even in a drag | Sliders, splitters, gizmo handles jumped when a drag left the window | The pointer keeps reporting while a button is held |
| 5 | An unfocused editor rendered at full rate | Wasted GPU | 16 ms idle when unfocused |
| 6 | No keyboard shortcuts besides undo/redo | Poor usability | Ctrl+S, Ctrl+D, Delete, W/E/R, F5; shown in the menus |

New tests: Win32 button/wheel messages, capture loss, gizmo shortcuts with and without Control, and click-after-move ordering.
The X11 path was not changed and not reviewed for items 2-4; item 1 is in the shared adapter and applies to both.

### Engine and editor gaps

| Defect | Fix |
|---|---|
| The editor's component catalog and the runtime's were **empty** | New module `engine/gameplay`: 13 built-in components (`docs/components.md`), registered by the editor and the runtime |
| Prefab assets were never loaded into the editor's library, so any scene with a prefab instance failed to open | The editor loads every `.prefab` of the project when it opens |
| `game_runtime` built an empty prefab library, so a packaged game with prefab instances could not start | It loads prefabs from the packs |
| Play created a physics world and a script runner but **bound no entity registry and created no bodies or script instances**: `transform.get/set` failed inside Play, and components did nothing | `GameWorld` (`bind_scene`, `sync_scene`): bodies from rigid body components, scripts from script components, pose write-back, and re-application of script-written transforms so the scene graph's dirty flags stay right. Play uses it |
| `game_runtime` loaded the scene, printed a line, and exited | It runs the scene through `GameWorld` for `--frames n` (default 60) at 60 Hz and fails on a script error |
| No sample or test project | `samples/showcase` (below) |
| `Scene` had no way to list entities or give a writable registry | `Scene::entity_guids()` and `Scene::mutable_registry()` |

One mistake of mine caught by the Linux Debug build: my first edit to `game_main.cpp` silently did not insert the simulation block, so the
Windows build passed while the runtime still only loaded the scene. The Linux `-Werror` build flagged the unused variable; the block is in
now, and the showcase test asserts the packaged game's output (`simulated 120 frames bodies=7 scripts=5`).

## The sample project

`samples/showcase`, generated by `tools/samples/generate_showcase.py` (deterministic, checked in). It opens in the editor, plays, and builds
into a package that launches from a clean folder.

| Area | Asset |
|---|---|
| Scene graph, hierarchy, prefab instances with overrides | `scenes/main.scene`, `prefabs/crate.prefab`, `scenes/credits.scene` |
| All 13 built-in components | used by `main.scene` (a test fails if one is unused) |
| Model import (glTF binary, OBJ) | `models/props.glb`, `models/marker.obj` |
| Texture import (color, normal, UI) | `textures/*.png` |
| Material graphs | `materials/*.material.json` |
| Animation state graph | `animation/hero.animgraph.json` |
| Lua scripting (lifecycle, state export/import, events, UI callbacks) | `scripts/*.lua` |
| Audio (WAV and OGG) | `audio/` |
| Terrain | `terrain/island.terrain` |
| Global illumination probes | `gi/probes.gi.json` (baked by the `gi_probes` importer during Build Game) |
| World partition (3 x 3 cells, always-loaded layer) | `world/` |
| Physics: static, kinematic, dynamic, trigger; box and sphere | `ground`, `platform`, `ball`, `zone` |

`tests/src/showcase_tests.cpp` (10 cases): the asset scan is clean; the scene loads with no missing asset and uses every component; a
3-second simulation drops the ball onto the ground with no script error; the editor opens the project and scene, plays, and stops; the
animation graph builds a state machine; the material graphs parse; terrain and audio load; the world opens; Build Game imports every asset
type it reaches, packs, launches from a clean copy, and the published launcher runs the simulation; and a coverage test names one asset per
feature area.

Writing the sample exposed more bugs: two scenes with non-unit quaternions (rounded constants), a material parameter with the wrong value
shape, a world cell grid that did not own its objects, and the runtime prefab problem above. The loaders caught all of them with clear
messages, which is a good sign for the formats.

## What is still not production ready

In the order I would do them.

1. **The runtime draws nothing and has no window.** The scene renderer, lighting, shadows, post, GI, ray tracing, particles, and terrain
   renderer exist as libraries with GPU tests, and the editor's own viewport draws wireframe/gizmos through ImGui draw lists. Nothing builds a
   `FrameSnapshot` from the mesh renderer, light, camera, and environment components. This is the largest gap between "engine" and "game".
2. **The editor was never driven by a human or a UI script.** Fixes above are from code review and headless tests. Someone should do the
   smoke test on an unlocked desktop (open the sample, click through the panels, drag a gizmo, Ctrl+Z, F5), and a screenshot-based test should
   replace the manual step. `docs/acceptance.md` still lists the visual smoke test as open.
3. **Components other than rigid body and script have no system behind them yet** (audio emitters, animator, particles, terrain,
   environment, UI canvas). They are edited, saved, packed, and loaded.
4. **Model import drops skeletons and animation clips** (`docs/model-import.md` lists them as unsupported notes), so the sample's animation
   graph can only run against a hand-built skeleton in tests.
5. **`audio.load` takes a file system path**, not an asset GUID, so a packaged game's scripts cannot load audio from the pack.
6. **Vulkan validation** cannot be reproduced from the repository (above). The native Linux Vulkan GPU suite and the acceptance benchmark are
   still not run. FSR 2 is unavailable. Linux GCC 11 is untested.
7. **No license file.** Needs your decision.
8. **`docs/build.md` says MSVC 14.38 is verified**; this tree configures with 14.51 (VS 18). Pick one and pin it in the presets.
9. **Editor polish not done:** no OS cursor shapes, the window does not redraw during a resize drag (a modal tick exists in the platform
   layer but the editor does not use it), numpad and punctuation keys have no ImGui mapping (text still arrives through `WM_CHAR`).
10. **Kinematic bodies** are placed where the scene puts them at start; `GameWorld` does not push a script's later moves into Jolt.
    A child of a dynamic body does not follow through physics, only through the scene graph.

## Files

New: `engine/gameplay/` (components, `GameWorld`), `samples/showcase/`, `tools/samples/`, `tests/src/showcase_tests.cpp`, `docs/components.md`,
`.gitattributes`, this report.
Changed: `engine/platform/src/win32/platform_win32.cpp`, `engine/editor/src/{imgui_input,editor_ui,editor_model,play_session,scene_editor,distribution}.cpp`,
`engine/runtime/src/{game_main,runtime_options}.cpp`, `engine/scene` (two accessors), `engine/rhi/src/vulkan/vulkan_device.cpp`,
`tools/game_editor/src/main.cpp`, tests, CMake wiring, and the docs (`editor.md`, `schemas.md`, `runtime-packs.md`, `acceptance.md`, `README.md`).

---

# Second pass (production-readiness follow-up)

You asked for the open items to be turned into working code, with tests, once the desktop was unlocked, the Vulkan SDK was installed, MIT was
chosen as the license, and the model importer was to be extended. This section replaces the "still not production ready" list above where it
says so. State: `sonnet` branch, all Windows suites green on DX12 and Vulkan.

## Results

| Check | Result |
|---|---|
| `engine_tests --suite unit` / `integration` (Release) | 1037 / 148 cases pass; Debug passed earlier |
| `ctest --preset win-release-unit` | 29 of 29 |
| `engine_tests --suite gpu --backend dx12` | **73 of 73** (43 942 assertions), RTX 5080, validation on |
| `engine_tests --suite gpu --backend vulkan` | **74 of 74** (43 954 assertions) with the SDK 1.4.363 layer (`VK_LAYER_PATH`). The earlier failures were only the old 1.3.224 layer |
| `engine_tests --suite editor --backend dx12` / `vulkan` | 111 of 111 on both |
| `tools/ui_tests/editor_ui_test.ps1` (real mouse and keyboard on the real window) | 0 failed checks |
| The packaged showcase, started with its own launcher in a window, DX12 and Vulkan | Draws the lit, shadowed scene; the kinematic platform turns between frames |
| Linux (WSL, clang 10, Debug, `-Werror`) | **30 of 30** on the final code (the runtime, renderer, and importer changes included) |

## What I did

1. **Unlocked-desktop editor testing found the real input bug.** Typed text never reached any text field: the Win32 message pump dispatched
   `WM_KEYDOWN` but never called `TranslateMessage`, so no `WM_CHAR` was ever produced. Earlier headless tests sent `WM_CHAR` by hand and
   hid it. Fixed. This bug also affected any game built on the platform layer.
2. **Editor usability**, from screenshots of the real window: scenes can now be opened from **File > Open scene** and by double click in
   Assets (before, you had to type an absolute path into a box in the Hierarchy tab); hierarchy rows say what each entity is instead of
   showing a bare GUID prefix; the left dock is wider; `--log <file>` mirrors the console for scripts.
3. **A real-input UI test** (`tools/ui_tests/editor_ui_test.ps1`): launches the editor, types a path, double-clicks a scene, selects,
   Ctrl+D / Ctrl+Z / Ctrl+Y / Delete / Ctrl+S, F5 twice, wheel, asserts on the log, saves screenshots to `artifacts/ui/`. Run it on an
   unlocked Windows desktop.
4. **Drawing in the runtime.** `game_runtime --window` (the package launcher passes it) opens a window and draws the scene through
   `SceneRenderer`: primary camera, directional/point/spot lights with shadows, tinted meshes, the environment's sky color. A new
   `GameRenderer` reads the built-in components and presents by copying the image into the swapchain, so there is one path for DX12 and
   Vulkan and no new shader. Swapchains gained the copy-destination usage on both backends. Tests: a GPU case per backend renders the showcase
   and checks the camera, lights, items, lit pixel share, and distinct objects; the package test runs the real launcher headless.
5. **Model importer: skeleton, skin weights, clips** (payload version 3; static models stay version 2). Per-vertex joints and weights
   survive the mesh cooker's vertex reordering. A new skinned, animated glTF fixture and generator are covered by import, round-trip,
   determinism, and rejection tests (bad joint index, weights, parents, key times, channel joints, truncation).
6. **Gameplay systems:** kinematic bodies now follow their entity, and audio emitters start playing in Play; both are tested on the showcase.
7. **License:** MIT (`LICENSE`), with a note that `third_party/` keeps its own licenses. **Build docs:** MSVC 14.51 and the
   same-Visual-Studio rule are stated. **Vulkan docs:** the SDK and `VK_LAYER_PATH` setup is in `docs/vulkan.md`.

## What is still not done

1. **Meshes in the runtime are primitives** (a box, or a sphere for a sphere body), tinted by the component. The cooked model payload reader
   lives in the converter, which the runtime must not link. The next step is to move `read_model_payload` into a runtime library and feed
   `MeshProvider`. The same step gives skinned playback, because the importer now writes the skeleton and clips.
2. **Textures and material graphs are not applied** when drawing: the renderer's default PBR material is used with the component tint.
   Material graph compilation needs the shader compiler at run time, so a cooked material form is needed first.
3. **Components without a system yet:** animator, particle emitter, terrain, UI canvas (their libraries exist and have GPU tests).
4. **No resize, fullscreen, or gamepad-driven menu in the runtime window**; the picture is placed at the top-left at multiples of 64 pixels
   of width. No game input mapping is bound to scripts in the windowed loop (the platform layer and action map exist).
5. **Entity names:** the reflection system has no string field kind, so entities cannot be named; the hierarchy shows components instead.
6. **Editor:** no OS cursor shapes, no redraw during a window resize drag, no gizmo drag test in the UI script. The X11 path was not
   re-reviewed for the mouse-message issues fixed on Win32.
7. **Not run:** the full acceptance benchmark, native Linux Vulkan GPU, FSR 2, Linux GCC 11, ASan/UBSan on the new code.
