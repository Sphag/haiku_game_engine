# Asset Converter

The asset converter (US-019) prepares source assets without the editor. `asset_converter` validates a project, imports its sources, and cooks them into files that the runtime reads. It is headless: nothing in it needs a window or a GPU. Code is in `tools/asset_converter/`. The library `engine_converter` holds the pipeline; the `asset_converter` executable is a thin command-line wrapper. The runtime never links either (FR-03).

## Commands

```text
asset_converter <validate|import|cook|watch> --project <dir> [--output <dir>] [--jobs <n>] [--report <file|->] [--max-passes <n>]
```

- `validate` scans the project and checks the sidecars, the importers, the settings, and the dependencies. It writes nothing.
- `import` runs each asset's importer into `<output>/<guid>.imported`. It does not cook.
- `cook` imports what is stale and writes `<output>/<guid>.cooked`.
- `watch` cooks, then cooks again when the sources change. See [source-watch.md](source-watch.md).
- `--project` is the project root, which holds the sources and their `.meta` sidecars (see [asset-registry.md](asset-registry.md)). Required.
- `--output` is required for `import` and `cook`.
- `--jobs` sets the number of workers. `0` or no value uses the hardware thread count.
- `--report <file>` writes the JSON report. `--report -` writes it to stdout. The human summary always goes to stderr.
- `--max-passes <n>` applies to `watch` only. It stops the watch after `n` passes.

Exit codes: `0` when no diagnostic has error severity, `1` when the report has errors, and `2` for a usage problem or a report that cannot be written.

## Machine-readable report

The report is JSON with the format name `engine.converter_report`, version `1`, and these keys: `command`, `project`, `output` (null for `validate`), `status` (`ok` or `failed`), `counts` (assets per status), `assets` (GUID order), and `diagnostics`.

Each asset has a `status`:

| Status | Meaning |
|---|---|
| `valid` | `validate` passed for the asset |
| `imported` | `import` ran the importer and wrote the imported file |
| `cooked` | `cook` wrote the cooked file |
| `up_to_date` | a cached import or cook was reused |
| `failed` | the asset has its own error |
| `blocked` | a dependency has an error, so the asset was not converted |

Each diagnostic has a stable `code`, a `severity` (`error` or `warning`), the asset GUID and source path when they apply, and a message. Codes are part of the format; new codes may be added, existing ones are not renamed.

| Code | Meaning |
|---|---|
| `SCAN_IO`, `SCAN_MALFORMED`, `SCAN_WRONG_FORMAT`, `SCAN_UNSUPPORTED_VERSION`, `SCAN_GUID_CONFLICT`, `SCAN_PATH_CONFLICT`, `SCAN_OTHER` | the project scan failed; no asset is converted |
| `UNREGISTERED_SOURCE`, `ORPHAN_METADATA` | warnings: a source without a sidecar, or a sidecar without its source |
| `UNKNOWN_IMPORTER` | the sidecar names an importer that is not registered |
| `INVALID_SETTINGS` | the importer rejects the settings |
| `SOURCE_READ_FAILED`, `SOURCE_CHANGED` | the source could not be read, or it changed after the scan |
| `IMPORT_FAILED` | the importer rejected the source bytes |
| `IMPORT_UNSUPPORTED` | warning: the importer skipped data it does not support, and the rest of the asset is converted (see [model-import.md](model-import.md)) |
| `MISSING_DEPENDENCY` | a dependency names a GUID that is not registered |
| `DEPENDENCY_CYCLE` | the asset is part of a dependency cycle |
| `DEPENDENCY_BLOCKED` | a dependency failed or is blocked |
| `PUBLISH_FAILED` | an output file or the output directory could not be written |
| `INTERNAL_ERROR` | a conversion task threw an unexpected exception |

## Importers

| Type | Settings | Behavior |
|---|---|---|
| `blob` | none | the payload is the source bytes |
| `text` | `normalize_newlines` (bool, default `false`) | rejects text that is not UTF-8; with the setting on, `\r\n` becomes `\n` |
| `json` | none | rejects text that is not JSON; the payload is the canonical form (sorted keys, no whitespace) |
| `model` | `scale` (number greater than zero, default `1`) | glTF/GLB, FBX, and OBJ through Assimp; see [model-import.md](model-import.md) |
| `texture` | `usage` (`color`, `normal`, or `linear`, default `color`); `format` (`auto` or `rgba8`, default `auto`) | PNG/JPEG decoded to every mip in the cooked texture payload; see [texture-import.md](texture-import.md) |

An audio importer arrives later through the same `Importer` interface (`asset_converter/importer.hpp`). Each importer has a `version()`, which is part of every cache key of that importer. An importer returns its payload together with the notes for the data it skipped; the notes become `IMPORT_UNSUPPORTED` warnings.

## Caching and incremental conversion

Each asset has a manifest, `<output>/<guid>.manifest.json`, that records its keys and the hashes of its output files.

- **Import key** (SHA-256 of canonical JSON): converter version, GUID, importer type and version, canonical settings, and the source's content hash.
- **Import notes**: the importer's skipped-data notes are stored in the manifest, so a reused import reports them again.
- **Cook key**: the converter version, the import key, and the cook key of each direct dependency. Because dependency keys chain, a change anywhere below an asset invalidates it.

Settings are canonicalized before they are hashed. A default spelled out (`{"normalize_newlines": false}`) and an omitted one (`{}`) give the same key.

An import is reused when its key matches and the imported file still hashes to the recorded value. A cook is reused when its key matches and the cooked file still hashes to the recorded value. A missing, unreadable, or changed file is treated as stale. A missing or malformed manifest is treated as absent.

Changing a setting, a source, an importer version, or a dependency re-cooks exactly the assets that depend on it. A second run with no changes reports every asset `up_to_date`.

## Output files and determinism

- `<guid>.imported`: the importer's payload.
- `<guid>.cooked`: the container. It starts with the magic `OWCK`, a little-endian `u32` container version (`1`), a `u64` payload size, and the payload. It holds no timestamps or paths. The writer and reader are in `engine/assets/` (`cooked_container.hpp`), so the runtime reads the same layout without linking the converter. See `docs/runtime-assets.md`.
- `<guid>.manifest.json`: the keys and the hashes above.

Identical inputs give identical bytes and identical hashes, whatever the worker count. The tests check this for `--jobs 1` against `--jobs 8`, and for two separate output directories.

## Publishing and parallel safety

- Every file is written to a unique temporary name beside its target, then renamed over it. Two tasks, or two processes, never share a temporary file, so a reader never sees a torn file. On Windows, the rename retries briefly if another process holds the target open.
- Each asset has its own output names, so tasks in one wave never write the same file.
- A failed import or cook writes nothing for that asset. The previous `.imported`, `.cooked`, and manifest stay in place. An asset that depends on a failed one is blocked and is not written either.
- Dependents run in waves by dependency level on the job system (`engine/core/jobs.hpp`), so a dependency is always converted before its dependents.

## Verification

- `tests/src/converter_mesh_tests.cpp`: mesh cooking (US-023). LOD 0 reproduces the source triangles and their attributes, meshlets stay within 64 vertices and 124 triangles and cover each LOD's triangles, the LOD chain halves and its error does not decrease, the bounds are exact, degenerate and empty meshes cook cleanly, and corrupt cooked data is rejected.
- `tests/src/converter_texture_tests.cpp`: the texture payload, the BC4/BC5 and BC7 codecs, mip chains, the texture importer on PNG and JPEG fixtures, corrupt input, and a PNG cooked through the converter.
- `tests/src/converter_tests.cpp`: unit cases for the importers and the registry, and integration cases for the fixture cook, the cache and transitive invalidation, determinism across worker counts, a failed import keeping the previous result, missing dependencies and cycles, unknown importers and bad settings, a malformed sidecar, a missing project, the import-only path, concurrent cooks into one directory, and the report format.
- `tests/src/converter_watch_tests.cpp`: the debouncer, the snapshot, the watch session on a scripted change source, and the OS watch on a scratch project (US-025).
- `tests/cli/`: exit codes and output of the built executable, against the checked-in fixture `tests/fixtures/converter/project`.

## Not in this story

- Audio importers and packs (US-057 and US-060).
- A source that changes during a conversion is caught by the content-hash check and reported as `SOURCE_CHANGED`. `watch` starts a new pass for it. A single `cook` run does not restart.
- Cross-process locking. Concurrent processes are safe because of the unique temporary files and deterministic bytes, not because of a lock; a process can redo work that another one already finished.
