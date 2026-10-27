# Component Reflection

In-house component metadata in `engine_scene` (`engine/scene/`). The inspector (US-062) and the scene format (US-017) read it. No external reflection library is used.

## Pieces

- `ComponentBuilder<C>` (`reflection.hpp`): describes one C++ component type. `field<&C::member>(name, default, range)` names a data member by pointer; the getter and setter are generated from it. `build()` validates the result and returns every problem found.
- `ComponentDescriptor`: plain data. Type ID, schema version, C++ type, and fields in declaration order. It also has `get_field`, `set_field`, `apply_defaults`, and `describes<C>()`.
- `ComponentCatalog`: registry of descriptors. `add()` validates a descriptor and checks it against the catalog. `find` looks up by type ID or C++ type. Editor and serialization code iterate `components()` and never name a component type.
- `validate()` and `check_value()`: the rules, usable on their own.

## Stable type IDs

A type ID is written by the author, for example `engine.transform` or `game.door_lock`. It is never derived from `typeid` or compiler output, so it does not change between builds or platforms.

- Dot-separated segments. Each segment starts with a lowercase letter and then uses lowercase letters, digits, or underscores. At most 128 bytes.
- The hash is FNV-1a 64-bit of the name (`type_id_hash`). Its test vectors are pinned in `tests/src/scene_reflection_tests.cpp`.

Field names follow the same rule without dots, at most 64 bytes. Field names are part of the saved format, so renaming one is a schema change.

## Supported field types

| C++ member type | `FieldKind` |
| --- | --- |
| `bool` | `Bool` |
| `std::int32_t` | `Int32` |
| `std::uint32_t` | `UInt32` |
| `float` | `Float32` |
| `std::array<float, 2>` / `3` / `4` | `Float32x2` / `x3` / `x4` |
| `core::Guid` | `Guid` |

Any other type fails to compile, through a `static_assert` in `FieldTraits`. Its kind is the index of its alternative in `FieldValue`.

## Defaults and constraints

- A default is typed by the member, so it cannot have the wrong kind.
- `Range{min, max}` takes inclusive bounds. Vector fields apply it to each component. Bool and Guid fields take no range.
- `validate()` reports these as errors:
  - `InvalidDefault`: a default that is not finite or lies outside the range.
  - `InvalidConstraint`: a range on bool or Guid, a non-finite bound, or `min > max`.
- Float bounds are compared at float precision, so a max of `0.1` accepts the float `0.1f`.
- `set_field` applies the same checks to edits. It reports `ValueTypeMismatch`, `NonFiniteValue`, `ValueOutOfRange`, or `UnknownField`, and the component is unchanged on failure.

## Other checks

- Type IDs and field names follow the naming rules above (`InvalidTypeId`, `InvalidFieldName`).
- Schema version is at least 1 (`InvalidSchemaVersion`).
- Field names are unique within a component (`DuplicateFieldName`).
- A descriptor without getters and setters is rejected (`MissingAccessor`).
- `ComponentCatalog::add` rejects, and leaves the catalog unchanged for:
  - `DuplicateTypeId`: the type ID is already registered.
  - `TypeIdHashCollision`: two different type IDs share a hash.
  - `DuplicateComponentType`: one C++ type is registered under two IDs.

## Registering a component from game code

```cpp
ComponentBuilder<WaterVolume> water("game.water_volume", 1);
water.field<&WaterVolume::depth>("depth", 2.0f, Range{0.1, 50.0});
water.field<&WaterVolume::flowing>("flowing", false);

auto built = water.build();
if (built.ok()) {
    catalog.add(std::move(built).value());
}
```

Code that iterates the catalog handles the new component without changes.

## Not yet covered

- Serialization of values (US-017). Reflection provides the names, kinds, and schema versions it needs.
- Inspector widgets (US-062). Reflection provides the metadata and the checked `set_field` they need.
- Defaults are not compared with the component's C++ member initializers. A descriptor can disagree with `T{}`.
- The catalog is not thread-safe. Register components at startup, before worker threads read the catalog.

## Tests

`tests/src/scene_reflection_tests.cpp` (`[unit]`, 16 cases) covers the FNV-1a vectors, name rules, field order and kinds, lookups by ID and by type, checked `set_field` and `get_field`, float bound precision, duplicate IDs and types, invalid defaults and constraints, malformed descriptors, missing accessors, a user-defined component used by generic editor code, Guid/vector/integer round trips, and pointer stability in the catalog.
