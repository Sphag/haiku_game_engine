# In-House RHI Contract

The engine's GPU API lives in `engine_rhi` (`engine/rhi/`). Scene, ECS, and game code use only these types. DX12 and Vulkan are implementations of the same contract. US-026 defines the contract and the CPU reference device. US-027 (DX12) and US-028 (Vulkan) add the GPU backends. US-029 adds the DX12 resource and descriptor model, and US-030 adds the same model on Vulkan; its rules are in this contract.

## Pieces

- `Handle<Tag>` (`handle.hpp`): index, generation, and device id. Typed aliases (`BufferHandle`, `TextureHandle`, `PipelineHandle`, and so on). A handle holds no backend pointer.
- `types.hpp`: formats, usage flags, resource states, descriptor and pipeline descriptions, render pass descriptions.
- `capabilities.hpp`: `Feature` (mesh shaders, indirect count, ray tracing, timestamp queries) and the `CapabilityReport`.
- `command_list.hpp`: `CommandList`, the recorded work for one queue.
- `device.hpp`: `Device`, the abstract device, and `create_device()`.
- `validation_device.hpp`: `ValidationDevice`, the CPU reference backend. It runs no GPU work and enforces every rule below.
- `swapchain.hpp`: `Swapchain` and its settings: acquire, present, resize, and the presentation policy.
- `src/dx12/`: the DX12 backend (US-027), built on Windows only. See `docs/dx12.md`.
- `src/vulkan/`: the Vulkan backend (US-028), built on Windows and Linux. See `docs/vulkan.md`.
- `src/object_lifetime.hpp`: the object table, object base, and per-queue timeline index that the GPU backends share.

## Lifetime

- A `destroy_*` call invalidates the handle at once. Later use returns `InvalidHandle`.
- Storage stays allocated while a submission that recorded the object is pending. The slot is reused only after `wait_fence` or `wait_idle` completes that submission.
- A list that references an object destroyed after recording is rejected at submit. A rejected submit changes no list.
- A slot's generation changes when it is freed, so a stale handle is rejected even after the slot is reused.

## Resource transitions

- Every buffer and texture is created in `Common`, except a `CpuRead` buffer, which starts in `CopyDestination`, and a swapchain back buffer, which starts in `Present`.
- `CpuRead` buffers are readback memory. They must also have `CopyDestination` usage and allow no other state.
- `CpuWrite` buffers are upload memory. They must also have `CopySource` usage, start in `CopySource`, and allow no other state. `CpuRead` and `CpuWrite` cannot be combined.
- `transition(resource, before, after)` must name the tracked state as `before`. A mismatch returns `StateMismatch`.
- `after` must be allowed by the usage flags (`Vertex` for `VertexBuffer`, `RenderTarget` for `RenderTarget`, and so on).
- Draws, dispatches, and copies check the state of every resource they use. A descriptor's resource must be in the state its binding type needs (`ConstantBuffer`, `ShaderRead`, or `UnorderedAccess`).
- Tracked state is shared by all lists in recording order, so a later list sees the effects of an earlier one.

## Command lists and queues

- A list is created open, recorded, closed with `close()`, and submitted once.
- A list runs only on the queue it was created for. Draws need `Graphics`, compute work needs `Graphics` or `Compute`, and copies run on any queue.
- Draws need an active render pass, a bound pipeline of the matching kind, target formats that match the pass, and every bound set and buffer in its required state.
- Descriptor sets must match the pipeline layout at their index. Every layout of the pipeline must be bound.

## Capabilities and feature gates

- `CapabilityReport::features` gives each feature one of three states:
  - `Unsupported`: the adapter does not have it.
  - `DisabledForTest`: the adapter has it, but the device was created with it forced off.
  - `Supported`: enabled. Only this state is enabled.
- `DeviceDesc::disabled_features` (or `ValidationDeviceDesc::disabled_features`) forces features off. Tests use this to check fallback paths.
- Gated commands fail with `FeatureDisabled`. These are mesh pipelines and `dispatch_mesh` (`MeshShaders`), `draw_indexed_indirect_count` (`IndirectCount`), and `write_timestamp` (`TimestampQueries`).
- Ray tracing (US-041, US-042) is implemented on DX12 and Vulkan. The CPU reference device reports it as unsupported.

## Copies, readback, and clears

- `copy_texture_to_buffer` copies mip 0 of a `CopySource` texture into a `CopyDestination` buffer. Each row is written at `copy_row_pitch(width, format)`, which is rounded up to 256 bytes. The destination offset must be a multiple of 512 bytes, and the buffer must hold the whole footprint. Depth formats are rejected.
- `read_buffer` copies bytes out of a `CpuRead` buffer. It fails with `NotReady` while a pending submission uses the buffer, so wait for its fence first.
- `copy_buffer_to_texture` is the reverse of `copy_texture_to_buffer`. It fills mip 0 of a `CopyDestination` texture from a `CopySource` buffer. The source offset must be a multiple of 512 bytes, and the buffer must hold the whole footprint.
- `upload_buffer` writes bytes into a `CpuWrite` buffer. It fails with `NotReady` while a pending submission uses the buffer. Wait for that fence before rewriting upload memory the GPU may still read. The reference device keeps no buffer contents, so `upload_buffer` returns `Unsupported` there after its argument checks.
- `RenderPassDesc::clear_color` clears every color target when the pass begins. Depth targets are not cleared.

## Presentation

- `create_device` returns a device with no surface. `Device::create_swapchain` binds one native window to it.
- A frame is: `acquire_back_buffer()`, record work that leaves the buffer in `Present`, `submit` it on the graphics queue, then `present()`.
- `acquire_back_buffer()` waits until the frame that last presented that buffer has completed, so at most `buffer_count` frames are in flight. It returns nothing while the surface is empty (minimized). The caller skips that frame.
- `present()` fails with `StateMismatch` if the buffer is not in `Present`, and `InvalidState` if nothing was acquired.
- `resize(width, height)` waits for the GPU to go idle and then recreates the back buffers. A zero dimension empties the surface.
- Destroy every swapchain before the device that created it.
- `PresentSettings` holds the policy. With `vsync` on, each present waits for the display. With it off, `allow_tearing` gives immediate presents when the adapter supports tearing. Otherwise vsync stays on. `Swapchain::present_settings()` reports what took effect.

## Fences and timestamps

- `submit` signals a fence value when the submission completes. `fence_value` returns the last completed value.
- `wait_fence` runs pending submissions in order until the value is reached. It fails when no pending submission can reach it.
- `queue_wait(queue, fence, value)` makes later work on `queue` wait on the GPU until `fence` reaches `value`. The CPU does not block. The value must already be submitted, so a wait never names work that does not exist. Use it to order a copy before a dispatch, or a dispatch before a readback, across queues.

## Descriptors and resource counts

- A descriptor set takes its bindings from a layout. A binding names a type (`ConstantBuffer`, `ShaderResource`, or `UnorderedAccess`), an array length, and a visibility. A layout must have at least one binding, and no two bindings may share an index.
- `write_buffer` and `write_texture` fill one array element. The resource's usage must allow the binding type. A constant buffer binding takes a buffer only.
- A descriptor set is bound with `set_descriptor_set`, and its layout must match the pipeline's layout at that index. Dispatches fail with `InvalidState` if a bound descriptor was never written, and with `StateMismatch` if its resource is not in the state the binding type needs.
- A destroyed set, layout, or pipeline invalidates its handle at once. Storage follows the same rule as other objects: it stays alive until the submissions that named it complete.
- `resource_stats()` returns live counts of buffers, textures, and descriptor sets, the shader-visible descriptors those sets hold, and the holds that keep objects alive for pending submissions. Repeated allocation and release should leave these counts where they started.
- `write_timestamp` stores a value when its submission completes. `read_timestamp` returns `NotReady` before that.
- The validation device's timestamps are nanoseconds from device creation, with a 1 GHz frequency.

## Backends

- `create_device(DeviceDesc)` creates the validation device, the DX12 device on Windows, and the Vulkan device on Windows and Linux. A backend that cannot run here returns `Unsupported` and never falls back to another device. DX12 returns `Unsupported` off Windows.
- `DeviceDesc::allow_software_adapter` lets a software adapter (WARP, or a CPU Vulkan device) be selected. It is off by default, and a run that uses one is not a GPU report.
- `DeviceDesc::debug_layers` turns on the backend's layer: the D3D12 debug layer with GPU-based validation, or the Khronos validation layer for Vulkan. `check_debug_messages()` returns the errors that layer recorded since the last call. The validation device has no layer, so it always returns ok.
- `DeviceDesc::adapter_index` selects an adapter in the backend's preference order. The default takes the first hardware adapter: feature level 12_0 on DX12, Vulkan 1.2 with timeline semaphores on Vulkan. Software adapters are never chosen by default.
- The validation device is single-threaded and runs one timeline. It does not model queue overlap.
- The validation device has no surface, so `create_swapchain` and `read_buffer` return `Unsupported` there. Those paths are tested on the DX12 and Vulkan backends by the GPU suites.
- Descriptor sets, descriptor layouts, and compute pipelines exist on DX12 from US-029, and on Vulkan from US-030. Graphics pipelines, vertex and index input, and indexed draws exist on both backends from US-033. `draw_indexed_indirect` and `draw_indexed_indirect_count` exist on both backends from US-037 (DX12 `ExecuteIndirect`, Vulkan `vkCmdDrawIndexedIndirect` and `vkCmdDrawIndexedIndirectCount`). Mesh pipelines and `dispatch_mesh` exist on both backends from US-038. A mesh pipeline takes an optional `ShaderStage::Amplification` (the task stage), a `ShaderStage::Mesh` stage, and a pixel stage, with no vertex input. `dispatch_mesh` needs an active pass and a bound mesh pipeline. DX12 uses `DispatchMesh`, and Vulkan uses `vkCmdDrawMeshTasksEXT` from `VK_EXT_mesh_shader`.
- **Ray tracing (US-041).** `create_bottom_level` and `create_top_level` make triangle acceleration structures, `write_top_level` sets a top-level instance list, and `build_bottom_level` and `build_top_level` build or refit them (`BuildMode`). `create_ray_tracing_pipeline` makes a ray tracing pipeline from one DXIL library with ray generation, miss, and closest-hit entries, and `set_ray_tracing_pipeline` and `dispatch_rays` launch it. A top-level structure is bound with `write_acceleration_structure` as a `ShaderResource`. Each call fails with `FeatureDisabled` when the RayTracing feature is off. DX12 (`docs/dx12.md`) and Vulkan (`docs/vulkan.md`) implement them. The validation device returns `Unsupported` for them. The validation device has no acceleration structures, so it reports them as `Unsupported` when simulated.
- **Graphics state (US-033).** `PipelineDesc` carries one vertex buffer layout for slot 0 (`vertex_stride` and `VertexAttribute`s; `R32G32B32Float` and `R32G32Float` are the vertex formats). Vertex attributes at locations 0 to 2 are position, normal, and UV (US-034)). Its attribute location is the shader input: Vulkan reads `layout(location)`, and DX12 matches it to the HLSL semantic `TEXCOORD<location>`. Triangle lists with LESS depth tests when a depth format is set. US-034 made the cull mode (`CullMode::None` or `Back`), alpha blending (`alpha_blend`, which needs blendable formats: `is_blendable` in `pipeline_rules.hpp`), and depth writes (`depth_write`) per pipeline, with defaults that keep the old state. Front faces are counter-clockwise on screen: DX12 sets `FrontCounterClockwise`, and Vulkan uses `VK_FRONT_FACE_COUNTER_CLOCKWISE`. `begin_render_pass` sets the viewport and scissor to the smallest target, with depth 0 to 1. `RenderPassDesc::clear_depth` clears the depth target, and it needs one. NDC y points up on DX12 and down on Vulkan, so a Vulkan shader consumes `coords::to_vulkan_clip()` of an engine projection (`docs/runtime-conventions.md`). `check_vertex_layout` (`pipeline_rules.hpp`) holds the vertex rules that every backend shares. `first_instance` is not portable: DX12's `SV_InstanceID` starts at zero whatever `first_instance` is, and Vulkan's `gl_InstanceIndex` includes it. Portable shaders draw with `first_instance` 0 and index their own per-instance data with the instance ID. When per-instance data must follow `first_instance` (an indirect draw names its item that way), put it in vertex slot 1: `PipelineDesc::instance_attributes` advance once per instance, and both backends start them at `first_instance` (US-037). Bind slot 1 with `set_vertex_buffer(1, …)`; a draw from a pipeline that reads it fails without that binding.
- Vulkan's shader resource and unordered access bindings are mutable descriptors (`VK_EXT_mutable_descriptor_type`), so a binding can hold a buffer or an image as the contract allows. A layout with such a binding fails with `Unsupported` on a device without the extension.

