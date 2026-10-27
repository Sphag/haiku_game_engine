# Vendor manifest and notices

Every third-party source the build uses is vendored under `third_party/`, pinned in `third_party/manifest.json`, and
checked by `cmake/ValidateVendor.cmake`. Configure and build never download anything; `tools/ci/offline_verify.sh`
enforces this on Linux.

## Checking the vendored tree

```sh
cmake -P cmake/ValidateVendor.cmake
```

The validator reads the manifest and checks, for each dependency:

- the required fields (`name`, `version`, `url`, `revision`, `license`, `licenseFile`, `root`) are present,
- the URL is `https://` and the revision is a 40-character commit hash,
- every listed file exists and its SHA-256 matches the manifest, and every listed patch matches its hash,
- no file exists under a vendored root that the manifest does not list (`unlisted file`).

The DXC generators run with `PYTHONDONTWRITEBYTECODE=1` so that they write no `__pycache__` into `third_party/`
(see [build.md](build.md)); the validator itself does not scan for Python caches.

The last successful run on the Linux verification machine reported: `Vendor validation passed: 26 dependencies, 8542 files`.

## Dependencies

| Dependency | Version or revision | License | Used for |
|---|---|---|---|
| Catch2 | v3.16.1 | BSL-1.0 | Unit test framework (`engine_tests`) |
| Dear ImGui (docking) | `d88f238d` | MIT | Editor and game UI tooling |
| Jolt Physics | master `01adafba` | MIT | Physics world, queries, character controller |
| Lua | 5.4.7 | MIT | Scripting VM (`lua_vm` behind its own target) |
| GLM | 1.0.1 | MIT | Math |
| stb (stb_truetype, stb_image, ...) | master `a7d4c217` | MIT OR Unlicense | Font rasterization and image helpers (behind a C shim) |
| bc7enc_rdo | master `b9438627` | MIT | BC7 texture compression (converter) |
| miniaudio | master `9634bedb` | MIT-0 OR Unlicense | Audio device and decode (behind a C shim) |
| nlohmann/json | 3.11.3 | MIT | JSON for scenes, sidecars, packs, saves |
| zstd | 1.5.6 | BSD-3-Clause OR GPL-2.0-only | Compression (the manifest records both license options) |
| ImGuizmo | master `105e4fde` | MIT | Viewport transform gizmos |
| imgui-node-editor | master `1e0e494f` | MIT | Material and animation graph editors |
| D3D12MemoryAllocator | master `b73737b0` | MIT | DX12 memory allocation |
| VulkanMemoryAllocator | 3.1.0 | MIT | Vulkan memory allocation |
| Vulkan-Headers | 1.4.363 | Apache-2.0 OR MIT | Vulkan API headers |
| Vulkan-Loader | 1.4.363 | Apache-2.0 | Loaded at run time; not linked |
| Vulkan-ValidationLayers | 1.4.363 | Apache-2.0 | Validation layer for Debug runs |
| Vulkan-Utility-Libraries | 1.4.363 | Apache-2.0 | Used by the validation layer build |
| SPIRV-Headers | 1.4.363 | MIT | SPIR-V headers |
| SPIRV-Tools | 1.4.363 | Apache-2.0 | Built with the shader toolchain |
| DirectX-Headers | pinned by DXC v1.10.2605.37 | MIT | D3D12 headers on Linux |
| DirectXShaderCompiler | v1.10.2605.37 | NCSA (LLVM Release License) | HLSL to DXIL and SPIR-V (built from source) |
| Assimp | 5.4.3 | BSD-3-Clause | Model import (converter only; the runtime link check excludes it) |
| meshoptimizer | master `f0a684f1` | MIT | Meshlet and mesh optimization |
| FidelityFX FSR 2 | master `1680d1ed` | MIT | Temporal upscaling source (see below) |
| Roboto | 2.x (`38062f4b`) | Apache-2.0 | UI font |

Entries marked `master` are pinned to the commit in their `revision` field (a full 40-character hash in the manifest), not to a release tag.

## Known gaps

- **FSR 2 is not usable yet.** The vendored tree lacks the eight `*_permutations.h` generated shader tables per backend, and
  the pinned archive does not include them either. `ENGINE_FSR2_AVAILABLE` in `engine/rhi/CMakeLists.txt` gates the build on
  them, and the backends return `Unsupported` without them. The route to a complete tree is an open decision.

## Notices

- `third_party/manifest.json` is the source of truth for licenses. Each dependency's `licenseFile` is vendored beside its sources.
- `NOTICES.txt` is generated at configure time from the same manifest, and every standalone game package carries a copy
  (see [distribution.md](distribution.md)). The editor's distribution step writes it into `<output>/<name>`.
- The repository has no top-level `LICENSE` file. The project's own license is an open decision for its owner, so no
  license is granted for the first-party code yet. The vendored dependencies keep the licenses listed above.

## Adding or updating a dependency

1. Add the source under `third_party/<name>` at the pinned revision, with no build output or Python caches in it.
2. Add its manifest entry: name, version, URL, revision, license, license file, vendored paths, excluded upstream paths,
   and a SHA-256 per file.
3. Run `cmake -P cmake/ValidateVendor.cmake`. It must pass before any configure.
4. Update this table and the README's licensing note if the license changes.
