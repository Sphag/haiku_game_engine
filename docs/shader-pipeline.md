# Shader Pipeline, Reflection, and Pipeline Cache

US-031 adds the shader pipeline of `engine/shader/` (library `engine_shader`). One HLSL source is compiled to DXIL for DX12 and to SPIR-V for Vulkan. Reflection of each target is checked against the declared layouts, a compiled-pipeline cache keys every input that can change the output, and a compute program can be reloaded without losing its last valid pipeline.

## Scope

Implemented and tested here:

- **Compile.** `ShaderCompiler` runs the pinned DXC that the build staged (`ShaderToolchain.cmake`). It never looks up `dxc` on PATH. Each compile uses `-Fc` for the listing and `-fvk-use-dx-layout` for SPIR-V, so CPU and GPU struct offsets follow the same HLSL packing rules on both targets. The include list comes from a second, preprocess-only pass (`-M`) that runs after a successful compile. A dependency flag on the compile itself (`-MF`, `-MD`) makes DXC stop before codegen and write no bytecode or listing.
- **Diagnostics.** Compiler output is parsed into `Diagnostic` records (severity, file, line, column, message). The indented source excerpt and caret lines that DXC prints under an error are context, not diagnostics. `diagnostics_to_text` gives compiler-style lines, and `diagnostics_to_json` gives the machine-readable report `engine.shader_diagnostics` version 1, with a `status` of `ok` or `error`. Both come from the same list.
- **Reflection.** The listings are parsed into one model: each binding has a class (constant buffer, shader resource, unordered access, sampler), a set, a register, and a count, and each constant buffer has member offsets and, for DXIL, its size. `validate_bindings` checks them against the declared `DescriptorSetLayoutDesc` list. `validate_cpu_layouts` checks the CPU struct (offsets taken with `offsetof`) against the shader. `compare_reflections` checks that the two targets agree.
- **Cache.** `PipelineCache` stores bytecode and reflection per request. The key is the request (source path, entry, profile, target, include directories, defines) and an environment string (compiler fingerprint, backend, adapter). Each entry also carries content stamps for the source and every include, so editing an include is a miss. The file records its environment, and a file written for another environment is rejected with an error. It is never merged.
- **Reload.** `ShaderProgram::reload` uses the cache when all stamps match. Otherwise it compiles, validates, and creates the new compute pipeline. The old pipeline is destroyed only after the new one exists. A compile error, a layout mismatch, or a failed `create_pipeline` keeps the last valid pipeline current and returns the diagnostics.

Not implemented here:

- **Graphics and mesh pipelines.** The RHI still returns `Unsupported` for them on DX12 and Vulkan. `ShaderProgram` builds compute pipelines only, and it rejects any non-`cs_*` profile. Graphics PSOs need vertex input, render-target state, and draws, which belong with the render graph and scene rendering (US-032, US-033).
- **Driver PSO blobs.** The cache stores compiled shader bytecode. It does not store `ID3D12PipelineLibrary` or `VkPipelineCache` blobs. Driver-side caches are not used yet.
- **DXIL samplers.** A sampler appears in reflection, but `validate_bindings` reports it, because the RHI descriptor model has no sampler type yet.
- **Unbounded arrays.** A runtime-sized descriptor array is rejected in SPIR-V reflection, because the RHI cannot describe it yet (bindless is a later story).

## Binding convention

The convention matches `dx12_compute.hlsl` and the RHI's `create_pipeline`. Descriptor set `i` is register space `i` in DXIL and `DescriptorSet i` in SPIR-V. A binding's register is its `Binding` number. A constant buffer, a shader resource, and an unordered-access resource map to `ConstantBuffer`, `ShaderResource`, and `UnorderedAccess`.

## Reflection source

Reflection reads the compiler's own listing (`dxc -Fc`), not the binary. For DXIL it reads the "Resource Bindings" table and the "Buffer Definitions" member offsets. For SPIR-V it reads the decorations, the type declarations, and the variables. The listing format is set by the vendored DXC, so a change to the pinned DXC is a change to these parsers. Their tests use real listings in `tests/src/shader_reflection_tests.cpp`, and `shader_compiler_tests.cpp` checks the same rules against real compiler output.

## Environment and invalidation

| Changed input | Effect |
| --- | --- |
| Source or include contents | Stamp mismatch: the entry is a miss. |
| Defines, entry, profile, target, include directories | The request key changes: a different entry. |
| Compiler executable or its libraries | The compiler fingerprint changes, and the cache file is rejected. |
| Backend or adapter | The environment changes, and the cache file is rejected. |

The compiler fingerprint is the SHA-256 of the DXC executable and the `dxcompiler` and `dxil` libraries beside it.

## Diagnostics format

Text is one line per diagnostic: `<file>:<line>:<column>: <severity>: <message>`. A diagnostic without a location is `<severity>: <message>`. JSON is:

```json
{"schema": "engine.shader_diagnostics", "version": 1, "source": "...", "status": "error",
 "diagnostics": [{"severity": "error", "file": "...", "line": 3, "column": 65, "message": "..."}]}
```

## Tests

- `[unit]`: `shader_diagnostics_tests.cpp`, `shader_reflection_tests.cpp`, `shader_cache_tests.cpp`. These run on every OS without a compiler.
- `[integration][shader]`: `shader_compiler_tests.cpp`, `shader_program_tests.cpp`. These run the pinned DXC and the CPU reference device.

GPU checks of the same compute path on DX12 and Vulkan are reported in `build.md` (US-031 row). They are not run on Linux, because this host has no Vulkan device.
