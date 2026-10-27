# Haiku Game Engine

A C++20 engine for open-world games, with an in-house Windows and Linux platform layer, a
DirectX 12 and Vulkan renderer, a Jolt-based physics world, Lua scripting, an asset converter,
resource packs, and an ImGui editor. Everything the build needs is vendored; configuration and
build never download anything.

**Status: pre-release.** The engine builds and its CPU unit tests run on the verified
platforms (see [Verified platforms](#verified-platforms)). Final acceptance is **incomplete**:
the real-GPU reports, the visual editor smoke tests, the clean-copy offline build, the standalone
package smoke tests on clean directories, and the full acceptance benchmark have not all been
produced. [docs/acceptance.md](docs/acceptance.md) lists each mandatory feature with its tests
and artifacts, and marks what is still open. Read it before relying on any feature.

## Documentation

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Module layers, the frame, and the ownership rules |
| [docs/capability-matrix.md](docs/capability-matrix.md) | What each platform and backend supports, and its verification state |
| [docs/acceptance.md](docs/acceptance.md) | Mandatory features mapped to tests and artifacts, with open items |
| [docs/build.md](docs/build.md) | Tools, presets, quality gates, the shader toolchain, CI |
| [docs/schemas.md](docs/schemas.md) | Scene, prefab, component, pack, save, and manifest schemas |
| [docs/vendor.md](docs/vendor.md) | Vendored dependencies, their pinned revisions, licenses, and notices |
| [docs/benchmarks.md](docs/benchmarks.md) | `engine_bench` and `engine_workload`, and how to reproduce a run |
| [docs/editor.md](docs/editor.md) | The editor workflow, input and focus rules, shortcuts |
| [docs/components.md](docs/components.md) | The built-in components and `GameWorld` |
| [samples/showcase](samples/showcase/README.md) | A sample project that uses every feature area, tested end to end |
| [docs/distribution.md](docs/distribution.md) | Building a standalone game package |
| [docs/asset-converter.md](docs/asset-converter.md), [docs/runtime-packs.md](docs/runtime-packs.md) | The asset pipeline and the pack format |

## OS and SDK requirements

### Windows

- Windows 10 or 11, x64. DirectX 12 and the Vulkan backend need a GPU driver for the device under test.
- Visual Studio 2022 or newer (MSVC 14.51 from Visual Studio 18 verified 2026-10-27, 14.38 earlier; always build from the same
  Visual Studio that configured the tree) with the "Desktop development with C++" workload and the
  "C++ ATL for latest v143 build tools" component. Configure from an x64 developer environment
  (`vcvars64.bat`).
- Windows SDK (provides `<windows.h>` and the D3D12 headers used by the build).
- CMake 3.28 or newer (4.3.4 verified), Ninja, and Python 3.8 or newer on `PATH`. The shader
  toolchain's generators run under Python.
- Vulkan GPU runs use the Vulkan SDK validation layer when the Debug build is run; see
  [docs/vulkan.md](docs/vulkan.md).

### Linux

- A native x86-64 Linux with glibc headers and `dlopen` (`libc6-dev`).
- Clang 10 with libstdc++ 9 is the verified local compiler (`linux-dev`, `linux-asan`). CI uses clang 14.
  GCC 9 cannot build Jolt; the `linux-gcc` preset needs GCC 11 or newer (CI uses gcc 11). Jolt uses
  `__builtin_bit_cast`.
- CMake 3.28 or newer, Ninja, and Python 3.8 or newer.
- Vulkan GPU runs need a Vulkan 1.2 driver with timeline semaphores, plus the validation layer for Debug runs.
- WSL and software rendering (llvmpipe, lavapipe, WARP, SwiftShader) run the CPU suites only. They do not
  produce GPU reports; see [docs/capability-matrix.md](docs/capability-matrix.md).

## Offline CMake setup

The configure step is offline by design. All sources are in `third_party/`, pinned in
`third_party/manifest.json`, and checked before configure:

```sh
cmake -P cmake/ValidateVendor.cmake      # checks every pinned file's SHA-256 and rejects stray files
cmake --preset linux-dev                  # or win-dev from an x64 developer prompt
cmake --build --preset linux-debug        # or win-debug
ctest --preset linux-unit --output-on-failure --no-tests=error
```

The DirectX Shader Compiler and its SPIRV-Tools dependency are built from source as part of the
default build (`ENGINE_BUILD_SHADER_TOOLCHAIN=ON`). Pass `-DENGINE_BUILD_SHADER_TOOLCHAIN=OFF` to
skip them while working on engine code; the shader tests are then not built.
`tools/ci/offline_verify.sh` runs the Linux clang gate inside a network namespace with no route
out, and fails if anything tries to download.

Presets, build targets, and the test presets are listed in [docs/build.md](docs/build.md).

## Verified platforms

| Platform | Compiler | Gate state |
|---|---|---|
| Windows x64 host (reference RTX 5080) | MSVC | See [docs/acceptance.md](docs/acceptance.md) for the latest gate results |
| Ubuntu 20.04 under WSL2 | Clang 10 | Configure, Debug build, and unit tests; no GPU device |
| Native Linux with a Vulkan GPU | — | Not yet run; no native report exists |

Do not read a platform as verified unless its gate and its GPU report exist.

## Editor workflow

The editor is `game_editor`. Build it with the default target, then:

1. Open or create a project. A project is a directory of assets, each with a sidecar that holds its GUID,
   importer, and settings.
2. Edit the scene in the hierarchy and inspector. Transforms are edited in the viewport with gizmos.
   Undo and redo cover every scene edit.
3. Import assets through the asset browser. Import runs the converter on a worker, with progress and cancel.
4. Use Play to run the scene in a separate session with its own physics, audio, and scripts. Pause, step,
   and stop from the toolbar. The session runs its own copy of the scene, so the authored scene is not changed by Play.
5. Use **File > Build game** to package the start scene (see [Native packaging](#native-packaging)).

Terrain sculpting, world partition editing, the material graph, and the animation graph each have a window.
[docs/editor.md](docs/editor.md) has the details, and [docs/world-editing.md](docs/world-editing.md),
[docs/terrain-editing.md](docs/terrain-editing.md), [docs/material-graph.md](docs/material-graph.md)
cover the three editors.

## Lua API

Scripts attach to entities as components. Each script runs in its own environment, and callbacks are read from
that environment only, so a global of the same name never runs. The lifecycle callbacks are the ones the script
defines; reloading compiles the new chunk before the old one is replaced, so a syntax error keeps the running
instance.

The engine exposes these modules:

| Module | Functions |
|---|---|
| `entity` | `create`, `destroy`, `alive`, `has`, `add`, `remove` |
| `transform` | `get`, `set` |
| `input` | `value`, `down`, `pressed`, `released` |
| `assets` | `find`, `exists`, `path`, `importer`, `dependencies` |
| `physics` | `raycast`, `overlap_sphere` |
| `animation` | `set_float`, `set_bool`, `trigger`, `get_float`, `get_bool`, `get_trigger`, `state`, `blending` |
| `events` | `on`, `off`, `emit` |
| `audio` | `load`, `play`, `play_at`, `stop`, `is_playing`, `set_position`, `set_listener`, `set_bus_volume`, `pause`, `play_music`, `stop_music` |

A binding that receives a bad argument raises a Lua error, and the instance is marked `Faulted`. Entities cross into Lua
as handles; each use checks that the entity is still alive. Script instance state is saved and restored by the save
system (`engine/save/`). The full binding reference is in the module documents: [docs/input.md](docs/input.md),
[docs/animation.md](docs/animation.md), [docs/runtime-conventions.md](docs/runtime-conventions.md), and the
headers under `engine/script/`. Game UI has its own Lua callbacks, described in [docs/ui.md](docs/ui.md).

## Asset pipeline

Sources are imported and cooked by `asset_converter`:

```sh
asset_converter validate --project <dir>                       # checks every sidecar, importer, setting, and dependency; writes nothing
asset_converter import   --project <dir> --output <out>        # runs each importer into <out>/<guid>.imported
asset_converter cook     --project <dir> --output <out>        # imports what is stale and writes <out>/<guid>.cooked
asset_converter watch    --project <dir> --output <out>        # cooks again when a source changes, until stopped
```

Each command takes `--jobs <n>` and `--report <file|->`. Cached imports and cooks are reused. A cancelled
cook publishes the assets it finished, and no file is left half-written.

`asset_packer` builds the resource pack for a start scene: a canonical JSON index behind a 48-byte header, and one
`<guid>.cooked` payload per asset. `PackReader::open` checks every payload's range, hash, and dependency closure before
use. A pack with a missing dependency is refused and nothing is written.

Importers: model import (`docs/model-import.md`), textures (`docs/texture-import.md`), mesh cooking
(`docs/mesh-cooking.md`), and the `json` importer for scenes and prefabs. The importer settings are canonical JSON in
each sidecar.

## Native packaging

**File > Build game** (or `engine::editor::build_game`) runs validate, cook, pack, package, verify, and publish. The result is a
folder with the platform's `game_runtime`, `data/game.pack`, the compiled shaders, `NOTICES.txt`, a launcher
(`run_game.bat` or `run_game.sh`), and `game.json`. The build starts the packaged runtime from an empty folder before it
publishes, so a package that does not start is never published.

Packages are native only: a Windows build makes a Windows package, and a Linux build makes a Linux package. The runtime links no
engine shared libraries. Full details, including the refusals, are in [docs/distribution.md](docs/distribution.md).

To check a package by hand, copy its folder somewhere empty and run the launcher there. Do not run it from the
project's directory: save data must never be written into an authored project.

## Benchmarks

`engine_bench` runs the suites declared in `benchmarks/manifest.json`. The `smoke` suite is a CPU self-check that runs on any
machine. The `acceptance` suite is the performance contract and needs a native backend:

```sh
engine_bench --suite smoke --output artifacts/bench/smoke-<id>
engine_bench --suite acceptance --backend <dx12|vulkan> --output artifacts/bench/<run-id>   # Release build only
```

The workloads are seeded and generated from the repository alone (`engine_workload --check benchmarks/workload_expected.json`).
[docs/benchmarks.md](docs/benchmarks.md) has the reproduction steps and the report format. No performance result has been
measured yet for the full acceptance suite.

## Tests

```sh
ctest --preset linux-unit --output-on-failure --no-tests=error     # CPU unit and integration tests
engine_tests --suite gpu --backend <dx12|vulkan>                   # real-GPU tests; need a GPU
engine_tests --suite editor                                        # editor model, serialization, and command checks
```

Sanitizers: `cmake --preset linux-asan`, `cmake --build --preset linux-asan`, and
`ctest --preset linux-sanitize`. Catch2 runs each suite by tag, so a single case cannot be selected by name.

## Licensing

The repository has no top-level `LICENSE` file yet; choosing the project's license is an open decision for its owner. Each
vendored dependency keeps its own license, listed in [docs/vendor.md](docs/vendor.md). `NOTICES.txt` is generated into every
game package from the same manifest.
