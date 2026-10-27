# Temporal upscaling (US-046)

The full profile renders the scene at a fixed 1280×720 and upscales to the 1920×1080 output with FSR 2 Quality. The native profile
renders at 1920×1080 with TAA and no upscaling. Both profiles compose the UI after upscaling, at the output size.

Status: **Partial, blocked.** The CPU contract and the RHI upscaler contract are implemented and tested. FSR 2 does not build yet: the
vendored tree lacks the generated shader tables it needs (see "Blocker"), so both backends report `Unsupported` from `create_upscaler`.
No frame goes through FSR 2 yet, and the renderer does not switch to the profile sizes.

## CPU contract (`engine/render/include/engine/render/upscaler.hpp`, `src/upscaler.cpp`)

- `profile_resolution(ImageProfile)`: the fixed sizes. `Native1080` is 1920×1080 without upscaling. `Full1080` is 1280×720 with
  `upscale = true`. The full profile's size is derived from the output and `kFsrQualityRatio` (1.5), truncated as FSR 2 does it
  (`fsr_render_extent`).
- `fsr_jitter_phase_count(render, display)`: `int(8 * (display / render)^2)`, as `ffxFsr2GetJitterPhaseCount`. At 1280→1920 this is
  18 phases. The TAA sequence of `postprocess.hpp` has 8 phases, so FSR 2 must not use `taa_jitter`.
- `fsr_jitter(frame, phase_count)`: the Halton(2, 3) sample of phase `frame % phase_count + 1`, centred on the pixel, in internal
  pixels. It matches `ffxFsr2GetJitterOffset`. Phase 0 is (0, −1/6).
- `benchmark_policy_allowed(UpscalePolicy)`: false when frame generation or dynamic resolution is on. The benchmark runs with both off.
- `UpscaleHistory`: the upscaler's own history. Its reset rule is the TAA rule (`taa_history_reset`): the first frame, a size change,
  and a flagged camera cut or teleport discard it. History counts only after `frame_done()`, so a frame that failed to dispatch does not
  count.

## RHI upscaler contract (`engine/rhi/include/engine/rhi/upscaler.hpp`)

- `Device::create_upscaler(UpscalerDesc)` makes an upscaler for a maximum render size and a display size. The upscaler is destroyed
  before its device. The CPU reference device returns `Unsupported`.
- `CommandList::upscale(Upscaler&, UpscaleDispatch)` records one upscale. The inputs keep their tracked states across the call:
  color, depth, motion, reactive, and exposure are `ShaderRead` with `ShaderResource` usage; the output is `UnorderedAccess` with
  `UnorderedAccess` usage. The formats are checked too (`check_upscale_texture`). The list then forgets its bound pipeline and sets,
  so the caller binds them again.
- `validate_upscale_dispatch` rejects a render size of zero or above the maximum, a jitter outside half a pixel, a non-positive
  pre-exposure, bad camera planes, and a field of view outside (0, π). Both functions are public and unit-tested (`rhi_upscaler_tests.cpp`).
- Motion vectors follow the engine's convention: UV units, x right and y down, with previous = current − motion (`scene.hlsl`). FSR 2
  reads previous = current + motion × scale / motion size, so the backend passes a negative scale equal to the render size.
- The depth is standard (0 at the near plane, 1 at the far plane), so FSR 2 gets no inverted or infinite-depth flag. The color is HDR
  linear, so FSR 2 gets `FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE`.

## Backends (`engine/rhi/src/fsr2_common.{hpp,cpp}`, `src/vulkan/vulkan_upscaler.*`, `src/dx12/dx12_upscaler.*`)

- `fsr2_common` builds FSR 2's context and dispatch descriptions from the RHI types. Its message callback counts errors, and a dispatch
  reports an error when the count rose.
- The Vulkan backend passes FSR 2 the device's own entry points. FSR 2's Vulkan backend calls five instance-level functions directly, so
  `vulkan_upscaler.cpp` defines them as forwarding shims. The RHI loads Vulkan at run time and links no loader, so this is the only way
  those calls resolve. The shims forward through a pointer that is set for the span of each FSR 2 call.
- The Vulkan and DX12 upscalers destroy their context after the device drains, because FSR 2 may still have work in flight.
- The FSR 2 sources and the backend upscalers are compiled only when all eight per-pass `*_permutations.h` headers exist for both
  backends (`engine/rhi/CMakeLists.txt`, `ENGINE_FSR2_AVAILABLE`). Otherwise the build says "FSR 2 is not built" and both backends
  return `Unsupported`.

## Blocker: FSR 2 shader tables are not vendored

The precompiled shader tables of FSR 2 (`vk/shaders/ffx_fsr2_shaders_vk.cpp`, `dx12/shaders/ffx_fsr2_shaders_dx12.cpp`) include eight
per-pass headers, `ffx_fsr2_<pass>_permutations.h`. Those headers hold the shader bytes and the permutation tables. They are generated
by FidelityFX's shader compiler at upstream build time. They are not in `third_party/fsr2`, and not in the pinned upstream archive
(`1680d1ed`, sha256 `19df8378…`). The archive does ship `tools/sc/FidelityFX_SC.exe`, but `third_party/manifest.json` deliberately
does not vendor prebuilt `tools/sc` binaries.

Two routes, which the user must choose (both change vendoring policy, so they are not taken unilaterally):

1. Run the archive's `FidelityFX_SC.exe` once on Windows, and vendor its generated headers with hashes in the manifest. This needs a
   documented exception for that one prebuilt tool. It gives upstream's exact output.
2. Regenerate the blobs with the engine's pinned DXC (DXIL and SPIR-V) through a first-party generator that writes the same tables. This
   keeps the no-prebuilt-tools rule, but it is more work and the bytes differ from upstream's.

Two more problems are in the vendored sources on Linux with clang and libc++: `ffx_fsr2.cpp` uses the MSVC macro `_countof`, and
`vk/ffx_fsr2_vk.cpp` uses `std::wstring_convert`. Both need local patches recorded in the manifest, or a compat header. The sources
target MSVC, and this tree has not compiled them with MSVC yet, because they are not built.

## Not yet wired

- **`SceneRenderer`.** It does not switch its targets to the profile sizes, and it does not run FSR 2 before the UI. The UI composition
  at the output size is therefore not done.
- **Reactive mask.** The engine writes no reactive mask yet. The upscale input is optional in the RHI, so the renderer can supply one
  after the scene's transparent pass is wired to write it.
- **GPU tests.** No GPU test runs an upscale, so the FSR 2 criterion "passes the GPU suite" is open.
- **Gates.** The Windows GPU and quality gates and the benchmark run are open for this story (see Verification).

## Verification

- Unit: `engine_tests --suite unit` passes, 581 cases, including `render_upscaler_tests.cpp` (9 cases) and `rhi_upscaler_tests.cpp`
  (3 cases).
- Linux clang: `ValidateVendor`, `linux-dev` configure, `linux-debug` build with no warnings, `linux-unit` ctest (exit 0).
- Not run on Linux: the GCC preset, the ASan and sanitize presets, the GPU suites (no Vulkan device here), and the benchmark.
- Windows: see the Windows gate results in `.ralph-tui/progress.md`.
