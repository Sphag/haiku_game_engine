# Asset Registry

The asset registry (US-018) gives every source asset a stable GUID, so a rename or move does not break a scene or another asset that refers to it. Code is in `engine/assets/`: `asset_registry.hpp` is the in-memory index, `asset_project.hpp` builds it from a project directory, and `content_hash.hpp` computes the digests.

## Sidecar metadata

Each source `<name>` has a sidecar `<name>.meta` beside it. The sidecar is the only place the GUID is stored.

```json
{
  "format": "engine.asset_meta",
  "version": 1,
  "guid": "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60",
  "importer": { "type": "texture", "settings": { "srgb": true } },
  "dependencies": [ "7c2e3b5f-1d4e-4f60-9b0c-2d3e4f5a6b71" ]
}
```

- `guid` identifies the asset. Scenes and other assets refer to it, never to the path.
- `importer.type` names the importer. `importer.settings` is a JSON object and is stored in canonical form.
- `dependencies` lists the GUIDs this asset needs. The importer that reads the source writes them (US-019). The registry stores and checks them; it does not derive them from the source bytes.

Unknown keys, a different format name, and a different version are errors.

## What the registry stores

For each asset: GUID, source path (relative to the project root), importer type, importer settings, the SHA-256 digest of the source bytes at the last scan, and the sorted list of dependency GUIDs.

The registry also answers these questions:

- `dependents(guid)`: which assets depend on a GUID.
- `missing_dependencies()`: every edge whose target is not registered.
- `dependency_closure(guid)`: everything reachable through dependencies. It is safe on cycles.

## Scan rules

`scan_project(root)` walks the tree and builds a new registry. It returns that registry only when every sidecar is valid. Any error returns nothing, so a failed scan never leaves a partial registry.

| Situation | Result |
| --- | --- |
| Source with a sidecar | Registered |
| Source without a sidecar | Listed in `unregistered`; not an error |
| Sidecar whose source is missing | Listed in `orphan_metadata`; not an error |
| Source moved or renamed with its sidecar | Same GUID, new path |
| Source copied with its sidecar | `GuidConflict`: two sources claim one GUID |
| Malformed sidecar | `Malformed`, `WrongFormat`, or `UnsupportedVersion` |

Symbolic links are not followed.

## Deletion, renaming, and missing dependencies

- **Rename or move a source with its sidecar.** The GUID survives. Every reference still resolves, because references name the GUID.
- **Move a source without its sidecar.** The file becomes a new, unregistered asset with a new GUID, and the old GUID is missing. Dependents report it through `missing_dependencies()`.
- **Delete an asset.** `AssetRegistry::remove` returns the dependents. Their edges stay, so they report the removed GUID as missing.
- **Delete only the source.** The sidecar is orphaned and the asset is not registered. The dependents report the GUID as missing.

Create a sidecar for an unregistered source with `create_asset_metadata`. It fails when the source is missing or already has a sidecar.

## Tests

- `tests/src/assets_registry_tests.cpp` (`[unit]`): SHA-256 vectors at the padding boundaries, record validation, GUID and path conflicts, dependency edges, removal, moves, and closures.
- `tests/src/assets_project_tests.cpp` (`[integration]`): the fixture project in `tests/fixtures/assets/registry_project` is copied to a scratch directory for each case. The cases cover registration, renaming, moving a folder, deletion, orphaned sidecars, GUID conflicts, malformed sidecars, and creating metadata.

## Not covered yet

- No dependency cycle check. The registry allows cycles and its closure terminates on them. The cooker (US-019) must reject them.
- No persisted index. The registry is rebuilt from the sidecars on each scan.
- Path comparison is exact. A case-only rename on a case-insensitive file system is not detected as a conflict.
