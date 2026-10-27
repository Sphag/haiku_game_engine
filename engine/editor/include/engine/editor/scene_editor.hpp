#pragma once

// The scene being edited (US-062): the hierarchy, the selection, the components, and the scene file. It has no ImGui or graphics
// dependency, so the commands are tested without a window (docs/editor.md).
//
// Structural edits (create, duplicate, remove, reparent, add or remove a component) change a copy of the scene document and load
// it back. Scene::load checks the whole document first, so a rejected edit leaves the scene as it was. Field and transform edits go
// straight to the scene, which checks them against the reflected descriptor. Every command that fails returns its error and writes
// it to the console; the inspector shows the same message next to the field.
//
// The hierarchy lists the scene's own entities (the document's). The inner entities of a prefab instance are not listed; the
// instance root shows its prefab. Those entities are still edited through the inspector when they are selected by GUID.
//
// The unsaved state is the difference between the scene's document and the text last saved or opened, so a field changed back to
// its saved value is clean again.

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/editor/console.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/transform.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace engine::editor {

// One line of the hierarchy: the entity, how deep it sits in the tree, and whether it is a prefab instance root.
struct HierarchyRow {
    core::Guid guid;
    std::size_t depth = 0;
    bool prefab_instance = false;
};

class SceneEditor {
public:
    explicit SceneEditor(std::shared_ptr<Console> console);
    SceneEditor(const SceneEditor&) = delete;
    SceneEditor& operator=(const SceneEditor&) = delete;
    SceneEditor(SceneEditor&&) = delete;
    SceneEditor& operator=(SceneEditor&&) = delete;

    // Component types. Game code registers its components here before the scene is opened or edited.
    [[nodiscard]] scene::ComponentCatalog& catalog() noexcept {
        return catalog_;
    }
    [[nodiscard]] const scene::ComponentCatalog& catalog() const noexcept {
        return catalog_;
    }
    [[nodiscard]] scene::PrefabLibrary& prefabs() noexcept {
        return prefabs_;
    }
    [[nodiscard]] const scene::Scene& scene() const noexcept {
        return scene_;
    }
    // The world matrix of an entity, copied out. Nullopt for an entity that is not in the scene. The viewport reads positions
    // through this, since the scene refreshes its cached matrices on demand.
    [[nodiscard]] std::optional<glm::dmat4> world_matrix(const core::Guid& entity);

    // The asset GUIDs the project holds. A Guid field outside this set is reported as a missing asset. The scene is checked again.
    void set_known_assets(std::unordered_set<core::Guid> assets);
    [[nodiscard]] bool is_known_asset(const core::Guid& asset) const;

    // The file the scene was opened from or saved to. Empty for a new scene that has never been saved.
    [[nodiscard]] const std::optional<core::Path>& path() const noexcept {
        return path_;
    }
    // True when the scene differs from the text last saved or opened.
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_;
    }

    // Clears the scene to an empty, unsaved one. Never fails.
    void new_scene();
    // Replaces the scene with the file's contents. A failed open (unreadable, malformed, or a missing prefab) leaves the scene as it was.
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> open(const core::Path& file);
    // Writes the scene to path(). Fails with Io when the scene has never been saved; use save_as for that.
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> save();
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> save_as(const core::Path& file);

    // Selection, shared by the hierarchy and the inspector. A selection that leaves the scene is cleared.
    [[nodiscard]] std::optional<core::Guid> selected() const noexcept {
        return selected_;
    }
    void select(std::optional<core::Guid> entity);

    // Rows in tree order (roots first, children in GUID order) or, when filter is not empty, the matching entities in GUID order.
    // A filter matches the GUID text or a component type ID, ignoring case.
    [[nodiscard]] std::vector<HierarchyRow> hierarchy(std::string_view filter) const;

    // Adds an entity with the identity transform under parent (nil for a root) and selects it.
    [[nodiscard]] core::Result<core::Guid, scene::SceneError> create_entity(const core::Guid& parent = {});
    // Copies an entity and its descendants under the same parent, with new GUIDs, and selects the copy. Entities inside a prefab
    // instance cannot be duplicated.
    [[nodiscard]] core::Result<core::Guid, scene::SceneError> duplicate(const core::Guid& entity);
    // Removes an entity and every entity below it. Clears the selection when it was among them.
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> remove(const core::Guid& entity);
    // Moves an entity under parent (nil for a root). Its local transform is kept, so it moves with the new parent.
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> reparent(const core::Guid& entity, const core::Guid& parent);

    [[nodiscard]] core::Result<core::Unit, scene::SceneError> add_component(const core::Guid& entity, std::string_view type_id);
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> remove_component(const core::Guid& entity, std::string_view type_id);
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> set_field(const core::Guid& entity, std::string_view type_id,
                                                                        std::string_view field_name, const scene::FieldValue& value);
    [[nodiscard]] core::Result<core::Unit, scene::SceneError> set_transform(const core::Guid& entity, const scene::Transform& local);

    // True when the entity is part of a prefab instance but is not its root. Its structure and transform come from the prefab.
    [[nodiscard]] bool is_instance_interior(const core::Guid& entity) const;

    [[nodiscard]] std::vector<scene::MissingReference> missing_references() const {
        return scene_.missing_references();
    }

    // Undo and redo (US-064). Every edit that changes the scene is one command. The history keeps the scene's document before and
    // after each command, so undo and redo restore the GUIDs, references, and components exactly. A new command clears the redo
    // steps. Opening or creating a scene clears the history. A rejected undo or redo leaves the scene as it was and logs why.
    [[nodiscard]] bool can_undo() const noexcept {
        return cursor_ > 0;
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return cursor_ < history_.size();
    }
    // The label of the command undo or redo would apply (for example "reparent"), empty when there is none.
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();

    // A gesture is a gizmo drag or a value edited in the inspector. Consecutive edits to the same target inside one gesture merge
    // into the command that began it, so the whole drag is one undo step. end_gesture closes the merge, so a later gesture on the
    // same target starts a new command. Both are idempotent.
    void begin_gesture();
    void end_gesture();

private:
    // One undoable command: its label, the scene's document before and after it, and the target it can merge into during a gesture.
    struct Command {
        std::string label;
        std::string merge_key;
        scene::SceneDocument before;
        scene::SceneDocument after;
    };

    // Loads the document into the scene. On failure the scene is unchanged and the error is logged.
    core::Result<core::Unit, scene::SceneError> apply(const scene::SceneDocument& document, std::string_view what);
    // Applies a structural edit and records it as one command labelled `what`. Nothing is recorded when the edit fails.
    core::Result<core::Unit, scene::SceneError> commit(const scene::SceneDocument& document, std::string_view what);
    // True when an edit with this key extends the last command instead of starting a new one.
    [[nodiscard]] bool merges(std::string_view merge_key) const noexcept;
    // Records an edit that has been applied to the scene. `before` is the document prior to it, and is unused when it merges.
    void record(std::string_view label, std::string merge_key, scene::SceneDocument before);
    void clear_history() noexcept;
    void update_dirty();
    void note(core::LogLevel level, std::string_view message);

    std::shared_ptr<Console> console_;
    scene::ComponentCatalog catalog_;
    scene::PrefabLibrary prefabs_{catalog_};
    scene::Scene scene_{catalog_, prefabs_};
    std::unordered_set<core::Guid> known_assets_;
    std::optional<core::Path> path_;
    std::string saved_text_;
    bool dirty_ = false;
    std::optional<core::Guid> selected_;
    std::vector<Command> history_;
    std::size_t cursor_ = 0;  // commands before this index are applied; the ones after it are redo steps
    bool gesture_open_ = false;
};

}  // namespace engine::editor
