#pragma once

// The versioned scene and prefab files (US-017). Both are JSON with a format name and an integer version, and both are
// read against a ComponentCatalog, because a field's JSON type comes from its reflected kind.
//
//   {
//     "format": "engine.scene",
//     "version": 1,
//     "entities": [
//       {
//         "guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
//         "parent": null,
//         "transform": { "position": [0, 1, 0], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1] },
//         "components": {
//           "game.door_lock": { "schema": 1, "fields": { "locked": true, "key": "<asset guid>" } }
//         }
//       },
//       {
//         "guid": "…", "parent": null,
//         "transform": { … },
//         "prefab": {
//           "guid": "<prefab asset guid>",
//           "overrides": [ { "entity": "<prefab-local guid>", "component": "game.door_lock", "field": "locked", "value": false } ]
//         }
//       }
//     ]
//   }
//
// A prefab file ("engine.prefab") has the same entity list, without "prefab" members, and exactly one root (the entity whose
// parent is null). Entity GUIDs inside a prefab are its local ids.
//
// Missing fields take their reflected defaults. Unknown keys, unknown components, unknown fields, and a schema number that
// differs from the catalog's are errors, so a file is never read partly. Asset GUID references are not checked here: they
// are Guid fields, and the scene reports the ones it cannot resolve (Scene::missing_references).

#include "engine/scene/reflection.hpp"
#include "engine/scene/transform.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scene {

inline constexpr std::string_view kSceneFormat = "engine.scene";
inline constexpr std::string_view kPrefabFormat = "engine.prefab";
inline constexpr int kSceneFormatVersion = 1;

enum class SceneErrorCode {
    Io,                        // the file could not be read or written
    Malformed,                 // not JSON, or the JSON structure is wrong, or a key is unknown
    WrongFormat,               // the format name is not the one this reader expects
    UnsupportedVersion,        // the file's format version is not this build's
    NilGuid,                   // an entity or prefab has the nil GUID
    DuplicateGuid,             // two entities, or a derived prefab entity, share a GUID
    MissingParent,             // a parent GUID names no entity in the file
    Cycle,                     // the hierarchy loops back on itself
    InvalidParent,             // the parent cannot hold this entity (a prefab-instance interior node, or a prefab instance)
    InvalidTransform,          // a transform has a non-finite value or a non-unit rotation
    UnknownComponent,          // no reflected component has this type ID, or the entity lacks it
    UnsupportedComponentSchema,  // the component's schema number differs from the catalog's
    UnknownField,              // the component does not declare this field
    InvalidValue,              // a value has the wrong JSON type, is non-finite, or is out of range
    InvalidPrefab,             // a prefab has no single root, a nested prefab reference, or other structural faults
    MissingPrefab,             // an instance names a prefab that the library does not hold
    InvalidOverride,           // an override names a missing entity, component, or field, or repeats one
    UnknownEntity,             // an operation names a GUID that is not in the scene
    InstanceEdit               // the operation changes a prefab instance's structure, which version 1 does not support
};

[[nodiscard]] std::string_view to_string(SceneErrorCode code) noexcept;

struct SceneError {
    SceneErrorCode code;
    std::string message;
};

// Values of one component, keyed by field name. A map keeps the output order deterministic.
struct ComponentData {
    std::uint32_t schema_version = 1;
    std::map<std::string, FieldValue> fields;

    friend bool operator==(const ComponentData& lhs, const ComponentData& rhs) {
        return lhs.schema_version == rhs.schema_version && lhs.fields == rhs.fields;
    }
    friend bool operator!=(const ComponentData& lhs, const ComponentData& rhs) {
        return !(lhs == rhs);
    }
};

// A field of a prefab instance that differs from the prefab. The entity is the prefab-local GUID.
struct PrefabOverride {
    core::Guid entity;
    std::string component;
    std::string field;
    FieldValue value;
};

// The prefab an instance comes from, and the overrides that differ from it.
struct PrefabReference {
    core::Guid prefab;
    std::vector<PrefabOverride> overrides;
};

// One authored entity. An instance root has a PrefabReference and no components of its own: its components come from the
// prefab. Its children are not in the file; they are rebuilt from the prefab.
struct EntityRecord {
    core::Guid guid;
    core::Guid parent;  // nil for a root
    Transform transform;
    std::map<std::string, ComponentData> components;  // keyed by component type ID
    std::optional<PrefabReference> prefab;
};

struct SceneDocument {
    std::vector<EntityRecord> entities;
};

struct PrefabDocument {
    std::vector<EntityRecord> entities;  // exactly one root; no PrefabReference inside a prefab
};

// Fills in reflected defaults for absent fields and checks every component against the catalog: its type, schema number,
// field names, and values. Used by the parsers and by Scene, so a document built in code gets the same checks.
[[nodiscard]] std::optional<SceneError> complete_components(std::map<std::string, ComponentData>& components,
                                                            const ComponentCatalog& catalog);

// Reads a scene or prefab from text. A parsed document has passed every check in complete_components.
[[nodiscard]] core::Result<SceneDocument, SceneError> parse_scene(std::string_view text, const ComponentCatalog& catalog);
[[nodiscard]] core::Result<PrefabDocument, SceneError> parse_prefab(std::string_view text, const ComponentCatalog& catalog);

// Text of a document. Output is deterministic: entities keep the order they are given, and components and fields are
// sorted by name. Floats are written as the shortest decimal that reads back as the same float; transforms as doubles.
[[nodiscard]] std::string serialize_scene(const SceneDocument& document);
[[nodiscard]] std::string serialize_prefab(const PrefabDocument& document);

// Writes the file atomically (core::write_file_atomic). The parent directory must exist.
[[nodiscard]] core::Result<core::Unit, SceneError> save_scene(const core::Path& path, const SceneDocument& document);
[[nodiscard]] core::Result<core::Unit, SceneError> save_prefab(const core::Path& path, const PrefabDocument& document);

// Reads and parses a file. A missing or unreadable file is an Io error. A failed load produces no document.
[[nodiscard]] core::Result<SceneDocument, SceneError> load_scene(const core::Path& path, const ComponentCatalog& catalog);
[[nodiscard]] core::Result<PrefabDocument, SceneError> load_prefab(const core::Path& path, const ComponentCatalog& catalog);

}  // namespace engine::scene
