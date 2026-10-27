# Benchmarks (engine_bench)

`engine_bench` runs the benchmark suites defined in `benchmarks/manifest.json` and writes a report folder. The manifest is the
versioned contract: seeds, scene workloads, image and physics profiles, route names, warmup, duration, and repeat counts all come
from it. Changing a workload means bumping `manifest_version`.

## Suites

- `smoke`: a CPU self-check of the harness (a seeded workload). It needs no GPU, applies no performance contract, and is the only
  suite that runs on any machine: `engine_bench --suite smoke --output artifacts/bench/smoke-<id>`.
- `acceptance`: the PRD performance contract on one native backend. It needs `--backend dx12` (Windows) or `--backend vulkan`
  (Windows or native Linux), and it uses the contract timing: 30 s warmup, 180 s measured, 3 repeats per scene/profile.
  `engine_bench --suite acceptance --backend <dx12|vulkan> --output artifacts/bench/<run-id>`.

Run acceptance in a Release build with validation layers off. A Debug build, or one with validation enabled, is reported as
incomplete and does not count.

## Report

`<output>/report.json` holds the host metadata (OS, CPU, memory, compiler, build type, adapter), the manifest hash and version, the
workload and profile definitions each run used, and for every run and repeat: status, reason, frames started, CPU and GPU
summaries (p50, p95, p99, max, mean), the max(CPU, GPU) frame summary used by the contract, each contract check, GPU and host
residency, and counters. Raw per-frame values are in `<output>/raw/*.csv` (`frame,cpu_ms,gpu_ms`).

CPU frame time is the start-to-start interval of each frame that starts after the warmup. Percentiles use nearest rank.

## Status and exit code

| Status | Meaning |
|---|---|
| `passed` | measured, and every contract check holds (smoke runs pass when they complete) |
| `failed` | measured, and a threshold was exceeded |
| `unsupported` | the backend, OS, or a profile's required feature is not available here |
| `missing` | the scene's workload is not implemented yet |
| `incomplete` | the run did not produce what the contract needs (no GPU timings, no residency, not Release, no frames) |

Exit codes: `0` every run passed; `1` a threshold failed; `2` usage or manifest error; `3` a run was unsupported, missing, or
incomplete, or the suite had no runs. Missing or unsupported work never exits `0`.

## Current state

The harness, manifest, contract evaluation, and report are implemented. `physics_stress` runs its headless profile on the CPU
(US-077, see `docs/physics.md`); its visual profile reports `missing`. `streaming_stress` runs its headless
streaming profile on the CPU (US-078, `tools/benchmark/src/streaming_stress.cpp`): the 200 000 descriptors over 32 x 32 cells, the
5400 m route at 30 m/s, and a separate teleport case. Its GPU profiles report `missing`. It runs in the `streaming_soak` suite, which
is a 30-minute paced soak and is kept out of acceptance. The other acceptance scene (`render_stress` is implemented for GPU runs;
`all_features`) is declared with its workload and reports `missing` until its story builds it. GPU timestamps and residency are not yet collected, so an acceptance run that
does execute reports `incomplete` until they are. On WSL, Vulkan has no usable device and DX12 is Windows-only, so acceptance
reports `unsupported` there. No performance result has been measured.

## Workloads (engine_workload)

`engine_workload` builds the scene content from each scene's seed and integer counts in `benchmarks/manifest.json`. It is the only
generator, and it runs from the repository alone: no download, no account, no private asset.

- Geometry: unique meshes (8 to 32 quads a side, seeded heights) and instance descriptors placed over the 8192 m world.
- Terrain: the shared world's detail window (9 x 9 tiles of 257 x 257 samples at 1 m) from a seeded value noise. Every scene
  shares it, because the world is shared.
- Animation: animated characters with a binary-tree skeleton and a 60-keyframe looping clip per character.
- Lights and suns, particle emitters (1000 live particles each), physics bodies, static colliders, constraints, physics
  characters, and audio emitters.

The generator is integer-only (millimetres, millidegrees, permille), so the bytes and their SHA-256 digests match on every
compiler and host. Route, timing, and runtime parameters (raycast rates, route length, the resident and active counters measured
at run time) are not generated; the table marks them as runtime keys.

`benchmarks/workload_expected.json` pins each scene's counts and one digest per component. `engine_tests` regenerates every scene
and compares it with that file. To check it from the command line:

```
engine_workload --check benchmarks/workload_expected.json
```

A changed digest is printed with its component. To accept an intended change, bump `kWorkloadVersion` in `tools/benchmark/src/workload.cpp`, rerun with
`--write benchmarks/workload_expected.json`, and say why in the commit.

The generator's own parameters (the mesh library size for scenes whose table names no meshes, the world seed, the noise
spacing) are part of the workload and are not in the table. The world block of the manifest must match the generator's world
constants, and `engine_workload` refuses a mismatch.

## Fixture provenance

`tests/fixtures/PROVENANCE.md` lists every fixture with its SHA-256, source, and license. The `engine_tests` unit case
`Fixtures: every file has provenance and an unchanged hash` reads it and fails on a missing, extra, or changed file. The audio
commands and the model and texture generators reproduce their files (the Ogg files decode to identical PCM, with different stream
serials).

The repository has no `LICENSE` file yet. Every fixture is project-authored, but the license the project grants is an open
decision for the owner, and the provenance table says so rather than naming one.

## Reproduction

The manifest, the seeds, the expected workload file, and the smoke workload are in the repository; nothing is downloaded. To check
a change to the contract, edit a copy of the manifest and pass it with `--manifest <file>`, then compare the report's
`manifest.sha256`. The same `--manifest` option works with `engine_workload`.
