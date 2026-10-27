# HDR postprocessing and TAA (US-039)

Two layers implement the story. `engine/render/include/engine/render/postprocess.hpp` and `engine/render/src/postprocess.cpp` are the CPU
reference: the formulas and the history rules. `engine/render/include/engine/render/post_processor.hpp` and
`engine/render/src/post_processor.cpp`, with `engine/render/shaders/postprocess.hlsl`, run the same formulas on the GPU. The scene
renderer uses the GPU path when `SceneRendererDesc::post.enabled` is set. `tests/src/postprocess_tests.cpp` checks the reference, and
`tests/src/render_postprocess_gpu_tests.cpp` compares the GPU output with it on DX12 and Vulkan.

## Frame flow when the post pass is on

1. The scene pass writes **linear HDR** color (`R16G16B16A16Float`), 8-bit entity IDs, depth values, and motion vectors. The pixel shaders
   skip the sRGB encode when the frame's linear-output flag is set (`scene.hlsl`, byte 232 of the camera slot).
2. The scene projection carries the **TAA jitter**, a sub-pixel NDC offset from `taa_jitter_ndc`. The vertex stage adds it to the rasterized
   position only, so the motion vectors stay unjittered. Shadow tiles are never jittered.
3. `PostProcessor::run` builds one render graph: TAA resolve, horizontal bloom, vertical bloom, tone map with the overlay, and a copy of the
   displayed 8-bit image into the color readback buffer. The graph waits for its submission, so `image().color` holds the frame when
   `render()` returns.

With the post pass off, the scene writes sRGB into an 8-bit target exactly as before. Golden fixtures and the material and lighting pixel
checks use that path, so they are unchanged.

## Jitter

- `taa_jitter(frame)` returns a Halton(2, 3) offset in pixels, centred on the pixel, in [-0.5, 0.5], y down. It repeats every `kJitterPhases` (8) frames.
- `taa_jitter_ndc(jitter, width, height, vulkan_clip)` converts it to an NDC offset. x is `2 * jx / width` on both backends. y is
  `2 * jy * tile_row_sign(vulkan_clip) / height`, so a row shifts by the same pixel amount on DX12 and Vulkan.
- `SceneRenderer` applies the jitter only when TAA is on (`PostProcessSettings::taa`). Its frame index advances with each rendered frame.

## Exposure

- `exposure_from_ev100(ev) = 1 / (1.2 * 2^ev)`.
- The GPU uses a manual exposure: `exposure_from_ev100(ev100 + ExposureSettings::ev_compensation)`, with `ev100` from
  `PostProcessSettings`.
- `ev100_from_luminance(L)` is the CPU reference of an automatic exposure (`log2(8 L)`, clamped to the settings' range, then the artist
  offset). A zero, negative, or non-finite luminance gives the darkest allowed EV. **The GPU does not compute automatic exposure yet**
  (it would need a luminance reduction and a readback); the reference is in place and tested.

## Color hygiene

`sanitize_color` replaces NaN and Inf with 0 and clamps negatives to 0. Every sample passes through it before bloom, tone mapping, or TAA,
on both the CPU and the GPU (`Sanitize` in `postprocess.hlsl`). `tests/src/render_postprocess_gpu_tests.cpp` puts NaN, Inf, and negative
samples next to a bright pixel and compares the displayed image with the CPU reference.

## Bloom

- `bloom_prefilter` is the soft-knee threshold. Below `threshold - knee` the contribution is 0. Across the knee it ramps quadratically.
  Above `threshold + knee` only the excess over the threshold survives.
- `bloom_blur_weights(kBloomRadius)` gives normalized Gaussian weights for 9 taps (`kBloomRadius` is 4), with sigma = radius / 2.
- The GPU blurs the prefiltered colors in two separable passes (horizontal, then vertical) at full resolution, with clamped edges. The
  CPU reference does the same, so the two agree within the half-float rounding of the intermediate target.

## Tone mapping and composition

`postprocess_pixel(scene, bloom, exposure, bloom_intensity)` computes `ACES((scene + bloom * intensity) * exposure)`. Bloom is added in
linear light before exposure, so it scales with the scene. The Narkowicz ACES fit is clamped to [0, 1].

`srgb_encode` gives the display value. The GPU applies it to the ACES output, the same curve the scene shader uses for the 8-bit target.

`composite_overlay(display, overlay)` blends straight-alpha overlay color over the encoded pixel, after tone mapping. It takes no exposure
argument, so an overlay never depends on scene exposure. The GPU suite shows the same overlay color at two exposures. The overlay is
set through `PostProcessor::set_overlay` or `SceneRenderer::set_overlay`; it starts fully transparent. The UI connection is US-058.

## TAA history

`TaaHistory` is the CPU reference for one render target. Its reset rule is `taa_history_reset`, which the GPU pass also uses.

- The reset reasons are `FirstFrame`, `Resize` (the size changed), `CameraCut` (flagged by the caller), and `Teleport` (the camera moved
  more than `kTeleportDistance`, 50 world units, in one frame, or the move is not finite). A reset frame resolves to its current color.
- `resolve(current, velocity, settings)` works per pixel:
  1. The current color is sanitized.
  2. The history is sampled at `pixel + 0.5 - velocity * size`. The velocity is in UV units (x right, y down), as `scene.hlsl` writes it.
     Off-target or non-finite motion rejects the history for that pixel.
  3. The history is clamped to the color box of the current 3x3 neighborhood (ghost rejection).
  4. The result is `mix(clamped_history, current, current_weight)`.
- The GPU keeps two HDR history targets and swaps them each run. It samples the history with `Load` at the same integer position as the
  CPU, so the two sample the same texel.

Where the scene camera moves, the GPU history resets: `PostProcessor::run` takes the camera position and a `camera_cut` flag, and
`SceneRenderer::notify_camera_cut()` sets the flag for the next frame. `SceneRenderer::resize` and `PostProcessor::resize` rebuild the
targets and reset the history (`HistoryReset::Resize`). `SceneRenderer::last_history_reset()` and `PostProcessor::last_reset()` report
the reason of the last frame.

## Motion on transparent surfaces

Alpha-blended items write no motion vector (scene.hlsl and `docs/materials.md`). For TAA they count as static, so a moving transparent
object can show ghosting. Opaque and alpha-masked surfaces are correct.

## Scope and what is not done

- The GPU post pass has no automatic exposure (see Exposure).
- The post pass runs at full resolution and does not upscale. FSR 2 and temporal upscaling are US-046 in the PRD.
- The reference overlay is composited from an uploaded image. The UI path is US-058.
- No benchmark: `engine_bench --suite acceptance` needs the bench target (US-074), which does not exist yet.

## Tests

- `postprocess_tests.cpp` (`[unit]`, CPU reference): jitter range, period, distinct phases, and the same pixel shift on DX12 and Vulkan;
  exposure and its clamps; NaN and Inf handling; bloom knee; blur normalization; ACES monotonicity and range; exposure scaling and bloom;
  overlay independence from exposure; sRGB encode; the reset rule; each reset reason; reset frames without blend; convergence on static
  content; a moving pixel followed by its history (with the correct motion) and rejected without it; off-screen motion; mismatched sizes;
  and teleport detection.
- `render_postprocess_gpu_tests.cpp` (`[gpu]`, DX12 and Vulkan with the debug layers): NaN, Inf, and negative inputs against the CPU
  reference; the overlay at two exposures; a moving block's history against `TaaHistory`, with the camera cut, teleport, and resize resets;
  and the scene renderer with the post pass (picks and depth from the scene targets, a tone-mapped red center, a black background, and the
  reset reasons).
