# Vulkan Device and Presentation

US-028 adds the Vulkan backend of the RHI contract (`docs/rhi.md`), and US-030 adds its resource and descriptor model. It lives in `engine/rhi/src/vulkan/` and is built on Windows and Linux. It compiles against the vendored Vulkan-Headers (`third_party/vulkan_headers`) as a system include, so the headers' warnings never reach `-Werror`. The loader is opened at run time (`vulkan-1.dll` on Windows, `libvulkan.so.1` on Linux), so no import library is linked and the build does not need a Vulkan SDK.

## What it does

- **Instance.** Vulkan 1.2 is requested. The platform surface extension is enabled when the loader offers it (`VK_KHR_win32_surface` or `VK_KHR_xlib_surface`). With `DeviceDesc::debug_layers`, `VK_LAYER_KHRONOS_validation` and `VK_EXT_debug_utils` are enabled and a messenger records every error message.
- **Adapter selection.** Physical devices are ranked discrete, then integrated, then virtual. A device qualifies when it reports Vulkan 1.2, has `VK_KHR_swapchain`, has a graphics queue, supports timeline semaphores, and supports `VK_KHR_synchronization2` (US-030). Software (CPU) devices are skipped unless `DeviceDesc::allow_software_adapter` is set. `DeviceDesc::adapter_index` picks from that list.
- **Queues.** A graphics queue family is required. Compute and copy take their own families when the adapter has them, and otherwise share the family above them. Each queue type has one timeline semaphore.
- **Lifetime.** The object-lifetime rules are shared with DX12 (`engine/rhi/src/object_lifetime.hpp`). A destroyed object leaves its handle table at once and is released when every timeline value that used it completes.
- **Submission and fences.** A submission signals its queue's timeline and, when given, the caller's fence (also a timeline semaphore). `wait_fence` and `fence_value` read those counters.
- **Swapchain.** A surface is created from the native window, then a swapchain with two or more images, on the graphics queue. The format must be B8G8R8A8 or R8G8B8A8 in sRGB non-linear space; the surface must offer it. `acquire_back_buffer` waits on an acquire semaphore. The first graphics submission after an acquire waits on it and signals the image's render-finished semaphore, and `present` waits on that. When a frame has no submission, `present` submits the sync alone, so the image is still ordered.
- **Out-of-date and suboptimal.** A suboptimal result, or an out-of-date result at present, marks the swapchain for a rebuild before the next acquire. An out-of-date result at acquire rebuilds it and retries once. A rebuild waits for the GPU, retires the old images and views, and passes the old swapchain to the driver.
- **Resize and minimize.** `resize` waits for the GPU and rebuilds. A zero dimension empties the surface and keeps the swapchain, so `acquire_back_buffer` returns nothing. A Win32 or X11 surface reports the window's size as its only legal extent, so the swapchain is built at that extent and `width()` and `height()` report it, even when `resize` asked for another size. A surface that reports no fixed extent takes the requested size.
- **Surface loss.** A lost surface is dropped with its images and rebuilt from the saved native window by the next acquire or resize. The frame that found the loss is skipped.
- **Presentation policy.** `PresentSettings::vsync` selects FIFO. With vsync off and `allow_tearing` on, an adapter with immediate-mode support presents with `VK_PRESENT_MODE_IMMEDIATE_KHR`. Otherwise vsync stays on. `present_settings()` reports the effective policy.
- **Images.** Each back buffer is moved from undefined to `Present` once, in a synchronous setup submission, when the swapchain is built. Images are tracked by the state the caller sees and by the layout the GPU holds, so every barrier starts from the right layout.
- **Readback.** `copy_texture_to_buffer` writes rows at the 256-byte pitch from `copy_row_pitch`. A row length in texels is that pitch divided by the texel size. `read_buffer` reads a `CpuRead` buffer, which is host-coherent memory, after its submission completes.
- **Timestamps.** `write_timestamp` resets and writes the query in the same submission. `read_timestamp` returns `NotReady` until the submission completes, then reads the value.
- **Validation.** `check_debug_messages` returns every error the messenger recorded since the last call. Warnings and performance messages are recorded by the layer but do not fail the check.
- **Errors.** `VK_ERROR_DEVICE_LOST` is returned as `DeviceLost`. Any other failed call is `InvalidState` and names the call and the Vulkan result.
- **Synchronization2.** Every barrier is a `vkCmdPipelineBarrier2KHR` and every submission is a `vkQueueSubmit2KHR`, with 64-bit stage and access flags. The extension and its feature are enabled at device creation. The entry points are loaded at run time, and creation fails with `Unsupported` if the driver does not export them.

## Resources and descriptors (US-030)

- **Upload and readback.** A `CpuWrite` buffer is host-visible and coherent, with `CopySource` usage, and starts in `CopySource`. It stays mapped for its whole life, so `upload_buffer` is a memcpy. It returns `NotReady` while a pending submission uses the buffer. A `CpuRead` buffer is mapped the same way, and `read_buffer` reads it after its fence completes.
- **Staging.** `copy_buffer_to_texture` copies mip 0 from a `CopySource` buffer into a `CopyDestination` texture with `vkCmdCopyBufferToImage`. Its rules match `copy_texture_to_buffer`: the source offset is a multiple of 512 bytes, and the buffer holds the whole footprint.
- **Layout transitions.** `transition` moves the tracked state and the image layout together, with one synchronization2 barrier per call. The layout comes from the state, so every barrier starts from the layout the GPU holds.
- **Descriptor layouts.** A `DescriptorSetLayoutDesc` maps one to one onto a `VkDescriptorSetLayout`. `ConstantBuffer` is a uniform buffer. `ShaderResource` and `UnorderedAccess` are `VK_DESCRIPTOR_TYPE_MUTABLE_EXT`, so one binding can hold a buffer or an image, as the RHI allows. The mutable list is `storage buffer` and `sampled image` for a shader resource, and `storage buffer` and `storage image` for an unordered access. A mutable binding needs `VK_EXT_mutable_descriptor_type`. Without it, creating such a layout fails with `Unsupported`, and constant-buffer layouts still work.
- **Descriptor sets and pools.** Sets are allocated from pool pages of 256 sets and 1024 descriptors of each kind. A page is chosen by its remaining budget. A page the driver reports as out of memory is retired, and the set moves to another page. Each set returns its budget when its storage is freed.
- **Writes.** `write_buffer` and `write_texture` fill one array element with `vkUpdateDescriptorSets`. A texture binding names its image layout from the binding type: shader-read for a shader resource, general for an unordered access. A texture written to a constant buffer binding is rejected.
- **Pipelines.** Compute pipelines are built from SPIR-V with one entry point. Before any Vulkan pipeline object is created, `create_pipeline` checks the adapter's limits against what the compute stage reads: the bound set count, uniform buffers, storage buffers, and images, using `VkPhysicalDeviceLimits`. A pipeline the adapter cannot bind fails with `Unsupported` before the driver sees it. Graphics pipelines (US-033) are built against a render pass with the same attachment formats as the passes they draw in. They use dynamic viewport and scissor state, fixed rasterization, and LESS depth tests. Mesh pipelines (US-038) build through the same render-pass path as graphics pipelines, with the task stage (`VK_SHADER_STAGE_TASK_BIT_EXT`) optional, the mesh stage, and the fragment stage. They take no vertex input.
- **Binding and dispatch.** `set_descriptor_set` checks the set's layout against the pipeline's layout at that index and binds it. `dispatch` checks every layout is bound, every descriptor is written, every resource is alive, and every resource is in the state its binding needs. Only a dispatch that passes marks its objects as used.
- **Queue waits.** `queue_wait` records a timeline wait. The next submission on that queue carries it, so the GPU orders the work and the CPU does not block. The fence is held until that submission is made.
- **Deferred destruction.** A destroyed set, layout, or pipeline leaves its table at once, like a buffer or texture. Its storage is freed when the submissions that used it complete. A set frees its Vulkan descriptor set from its pool at that point, and the pool counts drop with it. `resource_stats()` reports the live counts, the shader-visible descriptors that live sets hold, and the retained holds.

## Choices

- **Dedicated allocations.** Buffers and images each get one allocation. DX12 uses committed resources (US-029), so both backends share one allocation model. The vendored VMA (`third_party/vma`) is not wired in, so suballocation is not implemented. A later story can add it behind the same handles.

## Choices

- Each render pass and framebuffer is created for the begin call that uses it and freed with its list. A cache keyed by attachment formats is a later optimization.
- Buffers and images each get one dedicated allocation. Suballocation and the vendored VMA belong to US-030.
- Timeline semaphores and synchronization2 are required. Drivers without them are reported unsupported; they are not emulated with binary fences or legacy barriers.
- `IndirectCount`, `MeshShaders`, and `RayTracing` are reported from hardware support, as DX12 does. Ray tracing (US-042) uses `VK_KHR_acceleration_structure` and `VK_KHR_ray_tracing_pipeline`, with `VK_KHR_ray_query`, `VK_KHR_pipeline_library`, and `VK_KHR_deferred_host_operations`, plus `bufferDeviceAddress`. The device enables them only when the adapter reports all of them. A forced-off feature returns `FeatureDisabled`.
- Acceleration structures and their builds: each structure owns its storage and scratch. Builds are ordered by a barrier before (earlier writes to the inputs and scratch) and after (the structure's writes to every later build, trace, and shader read). The shader table is host-visible and written once at pipeline creation: one handle record per group, padded to the handle alignment, with each region on the base alignment. The library is the SPIR-V that dxc writes from the same HLSL as the DXIL, and each stage names its entry point.

## What it leaves to later stories

- **Graphics (US-033).** Vertex and index buffers bind with `vkCmdBindVertexBuffers` and `vkCmdBindIndexBuffer`, and `draw_indexed` records `vkCmdDrawIndexed`. The viewport and scissor are set after each `vkCmdBeginRenderPass` to the smallest target, with depth 0 to 1, and `clear_depth` sets the depth attachment's load op to `CLEAR`. The NDC y flip for Vulkan is applied by the scene renderer through `coords::to_vulkan_clip()`, not by a negative viewport.
- Indirect draws use `vkCmdDrawIndexedIndirect` and `vkCmdDrawIndexedIndirectCount` (US-037). Per-instance input is vertex binding 1 with `VK_VERTEX_INPUT_RATE_INSTANCE`, which starts at a draw's `firstInstance`.
- Mesh pipelines and `dispatch_mesh` (`vkCmdDrawMeshTasksEXT`) are added by US-038. The device enables `VK_EXT_mesh_shader` with its task and mesh features, and `vertexPipelineStoresAndAtomics` for the task stage's counters, only when the adapter reports all three. Ray tracing is US-041 (DX12) and US-042 (Vulkan).
- Bindless (runtime-sized) descriptor arrays are not in the RHI contract, so the backend does not enable descriptor indexing.

## Running the GPU suite

```text
engine_tests --suite gpu --backend vulkan
```

The suite runs only tests tagged `[gpu][vulkan]`. Each device is created with the validation layer on, and any error the layer reports fails the test. `VK_LAYER_PATH` must point at the layer's manifest when the SDK does not register it.

On a host that cannot run the backend, the command exits 3 and prints `unsupported` with the reason: no loader, no validation layer, no display, or no hardware adapter with timeline semaphores. A software adapter runs only when `ENGINE_GPU_ALLOW_SOFTWARE=1` is set. That is a development aid, and its run is not a GPU report.

`tools/ci/gpu_run.py --backend vulkan` wraps the same command for the self-hosted runner and records `passed`, `failed`, or `unsupported`.

## Verification record

Verified 2026-10-23 for US-030 (Windows RTX 5080, Khronos validation layer 1.3.224.0, `VK_LAYER_PATH` set):

- `engine_tests --suite gpu --backend vulkan` passed: 14 test cases, 520 assertions, with no validation errors. The US-030 cases port the DX12 US-029 fixtures to the same compute shader, compiled to SPIR-V.
- `engine_tests --suite gpu --backend dx12` passed: 13 test cases, 508 assertions, as the cross-check.
- Linux (this WSL host): `--suite gpu --backend vulkan` exits 3 `unsupported`. The native Linux GPU report is still not run.

Verified 2026-10-22:

- Windows (RTX 5080, Khronos validation layer 1.3.224 from the VulkanSDK, `VK_LAYER_PATH` set): `engine_tests --suite gpu --backend dx12` passed (8 cases, 71 assertions); `--backend vulkan` passed (9 cases, 80 assertions), with no validation errors.
- Linux (this WSL host): `--suite gpu --backend vulkan` exits 3 `unsupported`. No usable Vulkan device exists here, so the native Linux GPU report is still outstanding.
