# Render graph (US-032)

`engine/render/` (library `engine_render`) describes a frame as passes that name the resources they read and write. The graph checks the
description, orders the passes, derives the state transitions, assigns transient resources to device allocations, and records the frame
into one command list. It talks only to the RHI contract (`engine/rhi/`), so the same graph runs on DX12, Vulkan, and the CPU reference
device.

## Declaring a frame

```cpp
engine::render::RenderGraph graph;
const ResourceId color = graph.create_texture("color", color_desc);     // transient
const ResourceId staging = graph.create_buffer("staging", staging_desc);
const ResourceId out = graph.import_buffer("readback", handle, usage, rhi::ResourceState::CopyDestination);

graph.add_pass("draw", PassKind::Graphics, {{color, Access::ColorWrite}}, [color](PassContext& ctx) {
    // begin_render_pass / draws; ctx.texture(color) resolves the physical texture
    return rhi::RhiStatus{};
});
```

- **Access** is what a pass does to a resource: `VertexRead`, `IndexRead`, `ConstantRead`, `ShaderRead`, `IndirectRead`, `StorageWrite`,
  `ColorWrite`, `DepthWrite`, `DepthRead`, `CopySource`, `CopyDestination`. The access selects the state the resource must be in, so a pass
  never names a state directly. Writes are the accesses for which `is_write()` is true.
- **PassKind** is `Graphics` or `Compute`. Compute passes may use the accesses that graphics and compute share. They cannot use vertex,
  index, indirect, or render-target accesses.
- **Transient** resources are created at `execute()` and released when it returns. **Imported** resources are caller-owned handles, with
  the usage flags the caller says they allow, an initial state, and an optional final state (an exit barrier restores it).

## What compile() checks

`RenderGraph::compile()` does not touch a device. It returns a `CompileResult` whose `errors` name the passes involved:

| Error | When |
| --- | --- |
| `DuplicateName` | two passes, or two resources, share a name |
| `InvalidResource` | an access names no resource of this graph, or an imported handle is invalid |
| `IncompatibleUsage` | the pass kind, the resource kind, or the usage flags do not allow the access, or a pass declares a resource twice |
| `MultipleWriters` | two passes write one resource |
| `ReadBeforeWrite` | a transient resource is read, and no pass writes it |
| `Cycle` | the dependencies form a loop; the message gives the loop, starting at its earliest-declared pass |

The rules are:

- Each resource has at most one writer. Every reader depends on that writer, so the order follows the data.
- An imported resource has content already, so it may be read without a writer.
- Passes are ordered by a stable topological sort. Among ready passes, the earliest declared goes first.

## Barriers, lifetimes, and aliasing

- Each access moves its resource's slot to the access's state. A barrier is emitted only when the state changes, and it records the
  state the slot is in at that point. A transient slot starts in `Common`.
- A transient resource lives from its first access to its last, measured in execution positions. Transient resources with the same
  layout (texture size, mips, format, usage; buffer size and usage) and disjoint lifetimes share one slot. The reused slot keeps the
  state its previous user left it in, so the next barrier starts from that state.
- An imported resource always has its own slot.

## Execution

`execute(device, graph)`:

1. Creates each transient slot and wraps each imported handle.
2. Records every pass into one graphics command list. For each pass it writes a begin timestamp, emits the pass's barriers, runs the pass
   record function, and writes an end timestamp. Exit barriers close the list.
3. Submits the list once on the graphics queue, and waits for its fence.
4. Reads the timestamps (only when the device has `TimestampQueries`) and releases the transient resources.

On a failure the error names the pass that rejected the recording. Transient resources are released on that path too.

Compute passes run on the graphics queue. The RHI shares every resource exclusively, so overlapping queues would need queue-ownership
transfers that this version does not record. Async compute is not part of US-032.

`execute()` is synchronous: it waits for the frame. The frame-level pipelining and the two frames in flight belong to the runtime loop
(US-033 onward).

## Diagnostics

- `ExecutionReport` holds per-pass CPU and GPU times in execution order, the total CPU recording time, the total GPU span (first pass
  start to last pass end), the barrier count, the number of transient allocations, and the number of reused transient resources. GPU
  times are empty on a device without timestamp queries; they are never reported as zero.
- `export_json(graph, report)` writes `engine.render_graph` version 1: passes with accesses, barriers, and timings; edges by pass name;
  resources with their slot and lifetime; slots; exit barriers; and the timing totals.

## Verification

- `tests/src/render_graph_tests.cpp` (`[unit]`, all operating systems): ordering, each rejection rule with the passes it names, barriers,
  aliasing and the state a reused slot starts from, exit barriers, the JSON export, execution on the reference device, reuse counts, and
  the cleanup after a failing pass.
- `tests/src/render_graph_gpu_tests.cpp` (`[gpu][dx12]`, `[gpu][vulkan]`): the same readback chain on the GPU, with the debug layers on.
  It checks the reference clear color read back from a graph that reuses a transient allocation, stable live counts over three
  executions, and the timing rule.
- Shared fixture: `tests/src/render_graph_fixture.hpp`.

Not covered by this story: `engine_bench` (no benchmark target exists yet), graphics and mesh pipeline binding (that belongs to the
scene rendering story, US-033), and async compute.
