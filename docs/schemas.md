# Schemas

Every file the engine reads or writes is versioned, and every reader refuses what it does not know. Unknown keys,
a different format name, and a different version are errors, so a file is never read partly. This page lists each
schema. The code that enforces it is named in each section.

## Scene (`engine.scene`, version 1)

Written and read by `engine/scene/include/engine/scene/scene_format.hpp`. The file is JSON:

```json
{
  "format": "engine.scene",
  "version": 1,
  "entities": [
    {
      "guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
      "parent": null,
      "transform": { "position": [0, 1, 0], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1] },
      "components": {
        "game.door_lock": { "schema": 1, "fields": { "locked": true, "key": "<asset guid>" } }
      }
    },
    {
      "guid": "…", "parent": null, "transform": { "…": "…" },
      "prefab": {
        "guid": "<prefab asset guid>",
        "overrides": [ { "entity": "<prefab-local guid>", "component": "game.door_lock", "field": "locked", "value": false } ]
      }
    }
  ]
}
```

| Key | Meaning |
|---|---|
| `guid` | Entity identity. Nil and repeated GUIDs are errors. |
| `parent` | The parent's `guid`, or `null` for a root. A missing parent or a cycle is an error. |
| `transform` | `position` (3), `rotation` (quaternion, 4, w first), `scale` (3). Missing values take the identity. |
| `components` | Keyed by component type ID. Each entry has `schema` (must equal the catalog's number) and `fields`. |
| `prefab` | Makes the entity a prefab instance: the prefab asset's `guid`, and `overrides` for its prefab-local entities. |

A missing field takes its reflected default. A Guid field that names an asset outside the known set is kept and
reported by `Scene::missing_references()`; it is not an error. A missing prefab is an error.

## Prefab (`engine.prefab`, version 1)

The same entity list, without `prefab` members, with exactly one root (the entity whose `parent` is `null`). Entity
GUIDs inside a prefab are its local IDs.

## Components

A component is a C++ type registered in a `ComponentCatalog` (`engine/scene/include/engine/scene/reflection.hpp`) with:

- a type ID such as `game.door_lock` (the catalog refuses duplicates and hash collisions),
- a schema number (an integer; the file must carry the same number),
- one entry per field: its name, a reflected kind, a default, and an optional numeric range.

Reflected field kinds (`FieldKind`):

| Kind | JSON type |
|---|---|
| `Bool` | boolean |
| `Int32`, `UInt32` | integer, within range |
| `Float32` | number |
| `Float32x2`, `Float32x3`, `Float32x4` | array of 2, 3, or 4 numbers |
| `Guid` | GUID string |

Any other C++ member type fails a `static_assert`, so a field cannot be reflected by accident.

The catalog is supplied by the host program. The editor and the runtime both register the built-in components
(`engine/gameplay`, [components.md](components.md)) and then any game types, so a scene that names a type the catalog does not
hold is refused rather than partly loaded. The reflection tests build a small catalog (`tests/src/scene_format_tests.cpp`,
`make_catalog`) that shows the registration pattern.

## Asset sidecar (`engine.asset_meta`, version 1)

Each source asset has a sidecar next to it, read by `engine/assets/src/asset_project.cpp`:

```json
{
  "format": "engine.asset_meta",
  "version": 1,
  "guid": "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60",
  "importer": { "type": "texture", "settings": { "srgb": true } },
  "dependencies": [ "7c2e3b5f-1d4e-4f60-9b0c-2d3e4f5a6b71" ]
}
```

- `guid` identifies the asset. Scenes refer to it, never to a path.
- `importer.type` names the importer; `importer.settings` is canonical JSON.
- `dependencies` lists the GUIDs the asset needs. The importer writes them; the registry checks them.

## Cooked and imported outputs

- `<output>/<guid>.imported`: an importer's result, reused while the source and settings are unchanged.
- `<output>/<guid>.cooked`: the runtime form that a pack stores. The cook cache is keyed by the source digest and settings.

## Resource pack (`OWPK`)

Layout: a 48-byte header (magic `OWPK`, version, index size, SHA-256 of the index), then the index (canonical JSON), then
one full `<guid>.cooked` payload per entry. `PackReader::open` (`engine/pack/`) verifies every payload's range, hash, and
dependency closure before use. Bad magic, a bad hash, or a missing payload is refused.

## Game save (`<slot>.save`)

A save is one JSON file per slot (`engine/save/src/save.cpp`). Keys: `format`, `version`, `build`, `save_id`, `checksum`,
and `body`. `checksum` is `crc32:` plus eight hex digits, computed over the body's compact dump. Before a write replaces a
valid save, the old file is kept as `<slot>.prev`. A failed write leaves the slot as it was.

## Game package (`engine.package`)

`game.json` in a packaged game folder (see [distribution.md](distribution.md)): `format`, `name`, `platform`,
`start_scene`, `backend`, `profile`, `arguments`, `packs`, and every other file's size and SHA-256.

## Benchmark manifest and workloads

`benchmarks/manifest.json` is the versioned benchmark contract: `manifest_version`, seeds, scene workload tables,
image and physics profiles, route names, warmup, duration, and repeat counts. Changing a workload means bumping
`manifest_version`. `benchmarks/workload_expected.json` pins each scene's counts and one digest per component
(`engine_workload --check`). The workload generator is integer-only, so digests match on every compiler.
[benchmarks.md](benchmarks.md) describes the report.

## Lua script graph and animation graph

- Animation state machines: the graph JSON in `engine/animation/src/state_graph.cpp`. Required keys include each
  transition's `priority`, and priorities are distinct per source state. A parse failure returns a `core::Result` error.
- Material graphs: `engine/material_graph/`, with the graph document and layout kept as separate documents; a move changes
  no cache key.

See [animation.md](animation.md) and [material-graph.md](material-graph.md) for their node and key references.
