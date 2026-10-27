# Scene Format and Prefabs

The scene format (US-017) is the saved form of a scene, and prefabs are reusable groups of entities. Code is in `engine/scene/`: `scene_format.hpp` reads and writes the files, and `scene.hpp` holds the runtime scene and the prefab library.

## Files

Both files are JSON with a format name and an integer version. The format is reflected in the file, so a reader never guesses at it.

- `"engine.scene"`, version 1: entities with GUIDs, parents, transforms, components, and prefab instances.
- `"engine.prefab"`, version 1: the same entity list, with exactly one root and no prefab instances inside it.

The structure and an example are in the header comment of `scene_format.hpp`.

Each component is written with its schema number (`"schema"`) and its field values (`"fields"`). A field that is missing takes its reflected default. Fields are looked up by name, so the field names are part of the format.

## Errors

Loading never reads a file partly. Each of these is an error, and the error names the entity or component involved:

| Case | Error |
| --- | --- |
| Not JSON, bad structure, unknown key | `Malformed` |
| Format name differs | `WrongFormat` |
| Version differs from this build | `UnsupportedVersion` |
| Unknown component type ID | `UnknownComponent` |
| Component schema differs from the catalog | `UnsupportedComponentSchema` |
| Unknown field, or a value of the wrong type or out of range | `UnknownField`, `InvalidValue` |
| Nil or repeated GUID | `NilGuid`, `DuplicateGuid` |
| Parent missing, or a cycle | `MissingParent`, `Cycle` |
| Instance names a prefab that is not in the library | `MissingPrefab` |
| Override names a missing entity, component, or field | `InvalidOverride` |

Unknown keys are errors, because a newer file read by an older build must not lose data silently.

**Missing asset references are not errors.** A Guid field that names an asset outside the known set loads and is kept. `Scene::missing_references()` lists it, and a later save writes it back. A missing prefab is an error, because the instance has no structure without it.

## Atomic writes and failed loads

- `save_scene` and `save_prefab` write through `core::write_file_atomic`, so a failed write leaves the previous file in place.
- `Scene::load` checks the whole document before it changes the scene. A failed load leaves the previous contents in place. Every other mutator also checks first.

## Prefabs

A prefab library (`PrefabLibrary`) holds validated templates by asset GUID. A template is checked once, when it is added: one root, every parent present and reachable, no cycles, no nested instances, valid transforms, complete components.

An instance is one scene entity with a `"prefab"` reference. In the file it holds only:

- the prefab GUID,
- the instance's own transform (its root's transform), and
- the overrides: each field whose value differs from the prefab.

The instance's children are not written. The runtime builds them from the prefab when the scene loads.

### Derived GUIDs

An entity inside an instance has a GUID derived from the instance GUID and the prefab-local GUID (`instance_entity_guid`). The derivation depends only on those two values, so a reference to an inner entity is the same after a save and load. The instance root keeps the instance GUID.

### Overrides

`Scene::set_field` on an inner entity records an override only when the value differs from the prefab. Setting the value back to the prefab's value removes the override. The document therefore stores the differences, not copies of the prefab's values.

## Version 1 limits

- An instance cannot gain or lose components.
- Inner entities of an instance cannot be moved, given new children, or re-transformed. Only the instance root has its own transform.
- A plain entity cannot be a child of an instance or of its root.
- Prefabs cannot nest other prefabs.
- A prefab library entry cannot be replaced, so an instance's prefab never changes under it. To change a prefab, add it under a new asset GUID.

## Tests

- `tests/src/scene_format_tests.cpp`: round trips with exact transforms far from the origin, defaults, each error above, and the atomic save with a failing write.
- `tests/src/scene_tests.cpp`: load and save of a hierarchy, missing references and their resolution, a failed load that changes nothing, the prefab library, instancing, overrides, reload of an instance, and refused edits.
