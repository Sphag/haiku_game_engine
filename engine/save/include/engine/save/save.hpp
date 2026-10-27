#pragma once

// Game saves (US-059). A slot is one JSON file, "<slot>.save", in a save directory:
//
//   {
//     "format": "engine.save",
//     "version": 1,
//     "build": { "engine": "0.1.0" },
//     "save_id": "<guid>",            // stable: a slot keeps its id when it is overwritten
//     "checksum": "crc32:1a2b3c4d",   // CRC-32 of the body's compact JSON text
//     "body": {
//       "scene": { ...an engine.scene document holding only the selected components... },
//       "scripts": [ { "entity": "<guid>", "chunk": "<name>", "state": { ...plain data... } } ],
//       "world": { "cells": [ "<engine.world_cell text>" ], "layer": null }
//     }
//   }
//
// What a save holds: the selected gameplay components (reflected field values and the transform), the plain data of each script
// instance, and the world cells changed since authoring. Entities, cells, and objects are named by GUID, so a save resolves against
// the authored content. Nothing else is written: no GPU resource, physics body, ECS handle, or pointer.
//
// Compatibility: a file is rejected, and nothing from it is applied, when it is not JSON, names another format or version, fails its
// checksum, or fails the scene or world checks. The engine version in "build" is recorded for diagnostics; the format version and the
// component schema numbers decide compatibility.
//
// Writes: a slot is replaced atomically (core::write_file_atomic). Before that, a valid save in the slot is copied to "<slot>.prev",
// so the last good save survives a corrupt or failed write; read_previous loads it. A failed write leaves the slot as it was.

#include "engine/save/save_error.hpp"
#include "engine/save/world_changes.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/scene/scene_format.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::save {

inline constexpr std::string_view kSaveFormat = "engine.save";
inline constexpr int kSaveFormatVersion = 1;

// The engine version that wrote the build. Recorded in every save.
[[nodiscard]] std::string_view build_version() noexcept;

// One script instance's state. `entity` is the GUID of the entity the instance is attached to, and `chunk` names the script, so a
// restore attaches the same chunk to the same entity before it applies `state`. `state` is the JSON object from capture_state.
struct ScriptSnapshot {
    core::Guid entity;
    std::string chunk;
    std::string state;
};

struct SaveData {
    core::Guid save_id;  // nil when a save is new; write() then generates one
    scene::SceneDocument gameplay;
    std::vector<ScriptSnapshot> scripts;
    WorldChanges world;
};

// A copy of the document with only the components whose type IDs are in `selected`. Every entity stays, so the hierarchy is intact.
[[nodiscard]] scene::SceneDocument select_components(const scene::SceneDocument& document, const std::set<std::string>& selected);

// The file text. Fails with InvalidContent when a script state is not a JSON object or a string is not UTF-8.
[[nodiscard]] core::Result<std::string, SaveError> serialize_save(const SaveData& data);

// Reads a save file's text. Every check runs before anything is returned, so a failed parse produces no data.
[[nodiscard]] core::Result<SaveData, SaveError> parse_save(std::string_view text, const scene::ComponentCatalog& catalog);

// The save directory. Slot names are 1 to 64 characters from [A-Za-z0-9_-].
class SaveSlots {
public:
    explicit SaveSlots(core::Path directory) : directory_(std::move(directory)) {}

    // Writes the slot and returns the save_id it holds. A nil save_id gets a new GUID.
    [[nodiscard]] core::Result<core::Guid, SaveError> write(std::string_view slot, SaveData data) const;

    [[nodiscard]] core::Result<SaveData, SaveError> read(std::string_view slot, const scene::ComponentCatalog& catalog) const;

    // The save that the last successful write replaced. NoSave when no save was replaced.
    [[nodiscard]] core::Result<SaveData, SaveError> read_previous(std::string_view slot, const scene::ComponentCatalog& catalog) const;

    [[nodiscard]] bool exists(std::string_view slot) const;

    // Removes the slot and its previous save. Missing files are not an error.
    [[nodiscard]] core::Result<core::Unit, SaveError> remove(std::string_view slot) const;

private:
    [[nodiscard]] core::Result<core::Path, SaveError> file_for(std::string_view slot, std::string_view suffix) const;

    core::Path directory_;
};

}  // namespace engine::save
