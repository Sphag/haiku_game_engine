#include "engine/editor/scene_editor.hpp"

#include "engine/gameplay/components.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

constexpr std::string_view kCategory = "scene";

using Children = std::map<core::Guid, std::vector<core::Guid>>;

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

const scene::EntityRecord* find_record(const scene::SceneDocument& document, const core::Guid& guid) {
    for (const scene::EntityRecord& record : document.entities) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

scene::EntityRecord* find_record(scene::SceneDocument& document, const core::Guid& guid) {
    for (scene::EntityRecord& record : document.entities) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

// Children keyed by parent GUID; roots sit under the nil GUID. The document is in GUID order, so each list is too.
Children children_of(const scene::SceneDocument& document) {
    Children children;
    for (const scene::EntityRecord& record : document.entities) {
        children[record.parent].push_back(record.guid);
    }
    return children;
}

// The entity and every entity below it.
std::set<core::Guid> subtree_of(const Children& children, const core::Guid& root) {
    std::set<core::Guid> members;
    std::vector<core::Guid> pending{root};
    while (!pending.empty()) {
        const core::Guid guid = pending.back();
        pending.pop_back();
        if (!members.insert(guid).second) {
            continue;
        }
        const auto found = children.find(guid);
        if (found != children.end()) {
            pending.insert(pending.end(), found->second.begin(), found->second.end());
        }
    }
    return members;
}

// The error for an edit that names an entity the document does not hold: an entity inside a prefab instance, or an unknown one.
scene::SceneError not_editable(const scene::Scene& scene, const core::Guid& entity, std::string_view action) {
    if (scene.contains(entity) && scene.prefab_of(entity)) {
        return {scene::SceneErrorCode::InstanceEdit, "entity " + entity.to_string() + " is inside a prefab instance and cannot " +
                                                         std::string(action)};
    }
    return {scene::SceneErrorCode::UnknownEntity, "entity " + entity.to_string() + " is not in the scene"};
}

bool matches(const scene::Scene& scene, const core::Guid& guid, const std::string& needle) {
    if (lower(guid.to_string()).find(needle) != std::string::npos) {
        return true;
    }
    for (const std::string& type_id : scene.component_types(guid)) {
        if (lower(type_id).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void append_tree(const Children& children, const std::map<core::Guid, const scene::EntityRecord*>& index, const core::Guid& guid,
                 std::size_t depth, std::vector<HierarchyRow>& rows) {
    const auto record = index.find(guid);
    rows.push_back({guid, depth, record != index.end() && record->second->prefab.has_value()});
    const auto found = children.find(guid);
    if (found == children.end()) {
        return;
    }
    for (const core::Guid& child : found->second) {
        append_tree(children, index, child, depth + 1, rows);
    }
}

}  // namespace

SceneEditor::SceneEditor(std::shared_ptr<Console> console) : console_(std::move(console)) {
    // The built-in components make the Add Component list useful from the first launch, and a scene saved here loads in the runtime.
    if (auto registered = gameplay::register_builtin_components(catalog_); !registered.ok() && console_ != nullptr) {
        for (const auto& error : registered.error()) {
            console_->add(core::LogLevel::Error, "scene", "built-in component: " + error.message);
        }
    }
    saved_text_ = scene::serialize_scene(scene::SceneDocument{});
}

void SceneEditor::set_known_assets(std::unordered_set<core::Guid> assets) {
    known_assets_ = std::move(assets);
    // Loading the scene again re-checks its asset references against the new set.
    static_cast<void>(apply(scene_.to_document(), "asset list"));
}

bool SceneEditor::is_known_asset(const core::Guid& asset) const {
    return known_assets_.contains(asset);
}

void SceneEditor::new_scene() {
    static_cast<void>(scene_.load(scene::SceneDocument{}, known_assets_));
    path_.reset();
    selected_.reset();
    saved_text_ = scene::serialize_scene(scene::SceneDocument{});
    dirty_ = false;
    clear_history();
    note(core::LogLevel::Info, "started a new scene");
}

core::Result<core::Unit, scene::SceneError> SceneEditor::open(const core::Path& file) {
    auto loaded = scene::load_scene(file, catalog_);
    if (!loaded.ok()) {
        note(core::LogLevel::Error, "cannot open scene " + file.str() + ": " + loaded.error().message);
        return core::failure(loaded.error());
    }
    // Scene::load reports a missing prefab with its GUID, and checks the whole document before it changes anything.
    auto applied = apply(loaded.value(), "open " + file.str());
    if (!applied.ok()) {
        return applied;
    }
    path_ = file;
    saved_text_ = scene::serialize_scene(scene_.to_document());
    dirty_ = false;
    clear_history();
    note(core::LogLevel::Info, "opened scene " + file.str());
    for (const scene::MissingReference& missing : scene_.missing_references()) {
        note(core::LogLevel::Warning, "entity " + missing.entity.to_string() + " names missing asset " + missing.asset.to_string() +
                                          " in " + missing.component + "." + missing.field);
    }
    return core::success();
}

core::Result<core::Unit, scene::SceneError> SceneEditor::save() {
    if (!path_) {
        scene::SceneError error{scene::SceneErrorCode::Io, "the scene has no file yet; use Save As"};
        note(core::LogLevel::Error, error.message);
        return core::failure(std::move(error));
    }
    return save_as(*path_);
}

core::Result<core::Unit, scene::SceneError> SceneEditor::save_as(const core::Path& file) {
    const scene::SceneDocument document = scene_.to_document();
    auto saved = scene::save_scene(file, document);
    if (!saved.ok()) {
        note(core::LogLevel::Error, "cannot save scene " + file.str() + ": " + saved.error().message);
        return saved;
    }
    path_ = file;
    saved_text_ = scene::serialize_scene(document);
    dirty_ = false;
    note(core::LogLevel::Info, "saved scene to " + file.str());
    return saved;
}

void SceneEditor::select(std::optional<core::Guid> entity) {
    selected_ = entity && scene_.contains(*entity) ? entity : std::nullopt;
}

std::vector<HierarchyRow> SceneEditor::hierarchy(std::string_view filter) const {
    const scene::SceneDocument document = scene_.to_document();
    std::vector<HierarchyRow> rows;
    const std::string needle = lower(filter);
    if (!needle.empty()) {
        for (const scene::EntityRecord& record : document.entities) {
            if (matches(scene_, record.guid, needle)) {
                rows.push_back({record.guid, 0, record.prefab.has_value()});
            }
        }
        return rows;
    }

    std::map<core::Guid, const scene::EntityRecord*> index;
    for (const scene::EntityRecord& record : document.entities) {
        index.emplace(record.guid, &record);
    }
    const Children children = children_of(document);
    const auto roots = children.find(core::Guid{});
    if (roots != children.end()) {
        for (const core::Guid& root : roots->second) {
            append_tree(children, index, root, 0, rows);
        }
    }
    return rows;
}

core::Result<core::Guid, scene::SceneError> SceneEditor::create_entity(const core::Guid& parent) {
    scene::SceneDocument before = scene_.to_document();
    auto created = scene_.add_entity(scene::Transform{}, parent);
    if (!created.ok()) {
        note(core::LogLevel::Error, "cannot create an entity: " + created.error().message);
        return core::failure(created.error());
    }
    update_dirty();
    record("create entity", {}, std::move(before));
    select(created.value());
    note(core::LogLevel::Info, "created entity " + created.value().to_string());
    return core::success(created.value());
}

core::Result<core::Guid, scene::SceneError> SceneEditor::duplicate(const core::Guid& entity) {
    scene::SceneDocument document = scene_.to_document();
    if (find_record(document, entity) == nullptr) {
        scene::SceneError error = not_editable(scene_, entity, "be duplicated");
        note(core::LogLevel::Error, "cannot duplicate: " + error.message);
        return core::failure(std::move(error));
    }

    const std::set<core::Guid> members = subtree_of(children_of(document), entity);
    std::map<core::Guid, core::Guid> fresh;
    for (const core::Guid& member : members) {
        fresh.emplace(member, core::Guid::generate());
    }
    const std::size_t original_count = document.entities.size();
    for (std::size_t i = 0; i < original_count; ++i) {
        const scene::EntityRecord& record = document.entities[i];
        if (!members.contains(record.guid)) {
            continue;
        }
        scene::EntityRecord copy = record;
        copy.guid = fresh.at(record.guid);
        // The copy of the entity keeps its parent; the copies of its descendants hang under the copies of theirs.
        copy.parent = record.guid == entity ? record.parent : fresh.at(record.parent);
        document.entities.push_back(std::move(copy));
    }

    const core::Guid copy_guid = fresh.at(entity);
    auto applied = commit(document, "duplicate");
    if (!applied.ok()) {
        return core::failure(applied.error());
    }
    select(copy_guid);
    note(core::LogLevel::Info, "duplicated entity " + entity.to_string() + " as " + copy_guid.to_string());
    return core::success(copy_guid);
}

core::Result<core::Unit, scene::SceneError> SceneEditor::remove(const core::Guid& entity) {
    scene::SceneDocument document = scene_.to_document();
    if (find_record(document, entity) == nullptr) {
        scene::SceneError error = not_editable(scene_, entity, "be removed on its own; remove its instance root instead");
        note(core::LogLevel::Error, "cannot remove: " + error.message);
        return core::failure(std::move(error));
    }

    const std::set<core::Guid> members = subtree_of(children_of(document), entity);
    std::erase_if(document.entities, [&members](const scene::EntityRecord& record) { return members.contains(record.guid); });
    auto applied = commit(document, "remove");
    if (!applied.ok()) {
        return applied;
    }
    note(core::LogLevel::Info, "removed entity " + entity.to_string() + " and " + std::to_string(members.size() - 1) + " below it");
    return core::success();
}

core::Result<core::Unit, scene::SceneError> SceneEditor::reparent(const core::Guid& entity, const core::Guid& parent) {
    scene::SceneDocument document = scene_.to_document();
    scene::EntityRecord* record = find_record(document, entity);
    if (record == nullptr) {
        scene::SceneError error = not_editable(scene_, entity, "be moved");
        note(core::LogLevel::Error, "cannot move: " + error.message);
        return core::failure(std::move(error));
    }
    record->parent = parent;
    // Scene::load rejects a parent that is the entity itself, one of its descendants, or inside a prefab instance.
    return commit(document, "reparent");
}

core::Result<core::Unit, scene::SceneError> SceneEditor::add_component(const core::Guid& entity, std::string_view type_id) {
    scene::SceneDocument before = scene_.to_document();
    auto added = scene_.add_component(entity, type_id);
    if (!added.ok()) {
        note(core::LogLevel::Error, "cannot add " + std::string(type_id) + ": " + added.error().message);
        return added;
    }
    update_dirty();
    record("add " + std::string(type_id), {}, std::move(before));
    return added;
}

core::Result<core::Unit, scene::SceneError> SceneEditor::remove_component(const core::Guid& entity, std::string_view type_id) {
    if (scene_.prefab_of(entity)) {
        scene::SceneError error{scene::SceneErrorCode::InstanceEdit,
                                "entity " + entity.to_string() + " is in a prefab instance, which cannot lose components in version 1"};
        note(core::LogLevel::Error, "cannot remove " + std::string(type_id) + ": " + error.message);
        return core::failure(std::move(error));
    }
    scene::SceneDocument document = scene_.to_document();
    scene::EntityRecord* record = find_record(document, entity);
    if (record == nullptr) {
        scene::SceneError error{scene::SceneErrorCode::UnknownEntity, "entity " + entity.to_string() + " is not in the scene"};
        note(core::LogLevel::Error, "cannot remove " + std::string(type_id) + ": " + error.message);
        return core::failure(std::move(error));
    }
    if (record->components.erase(std::string(type_id)) == 0) {
        scene::SceneError error{scene::SceneErrorCode::UnknownComponent, "entity " + entity.to_string() + " has no " + std::string(type_id)};
        note(core::LogLevel::Error, error.message);
        return core::failure(std::move(error));
    }
    auto applied = commit(document, "remove component");
    if (applied.ok()) {
        note(core::LogLevel::Info, "removed " + std::string(type_id) + " from " + entity.to_string());
    }
    return applied;
}

core::Result<core::Unit, scene::SceneError> SceneEditor::set_field(const core::Guid& entity, std::string_view type_id,
                                                                    std::string_view field_name, const scene::FieldValue& value) {
    const std::string label = std::string(type_id) + "." + std::string(field_name);
    std::string key = "field:" + entity.to_string() + ":" + label;
    // A merging edit does not need the before-state: the command it extends already holds it.
    scene::SceneDocument before = merges(key) ? scene::SceneDocument{} : scene_.to_document();
    auto set = scene_.set_field(entity, type_id, field_name, value);
    if (!set.ok()) {
        note(core::LogLevel::Warning, "rejected " + label + ": " + set.error().message);
        return set;
    }
    update_dirty();
    record(label, std::move(key), std::move(before));
    return set;
}

core::Result<core::Unit, scene::SceneError> SceneEditor::set_transform(const core::Guid& entity, const scene::Transform& local) {
    std::string key = "transform:" + entity.to_string();
    scene::SceneDocument before = merges(key) ? scene::SceneDocument{} : scene_.to_document();
    auto set = scene_.set_transform(entity, local);
    if (!set.ok()) {
        note(core::LogLevel::Warning, "rejected the transform of " + entity.to_string() + ": " + set.error().message);
        return set;
    }
    update_dirty();
    record("move " + entity.to_string(), std::move(key), std::move(before));
    return set;
}

std::string_view SceneEditor::undo_label() const noexcept {
    return can_undo() ? std::string_view(history_[cursor_ - 1].label) : std::string_view{};
}

std::string_view SceneEditor::redo_label() const noexcept {
    return can_redo() ? std::string_view(history_[cursor_].label) : std::string_view{};
}

std::vector<std::string> SceneEditor::history_labels() const {
    std::vector<std::string> labels;
    labels.reserve(history_.size());
    for (const Command& command : history_) {
        labels.push_back(command.label);
    }
    return labels;
}

bool SceneEditor::go_to_history_position(std::size_t position) {
    position = std::min(position, history_.size());
    while (cursor_ > position) {
        if (!undo()) {
            return false;
        }
    }
    while (cursor_ < position) {
        if (!redo()) {
            return false;
        }
    }
    return true;
}

bool SceneEditor::undo() {
    if (!can_undo()) {
        return false;
    }
    // The command being undone can no longer merge with a gesture, so the gesture closes first.
    end_gesture();
    const Command& command = history_[cursor_ - 1];
    if (!apply(command.before, "undo " + command.label).ok()) {
        return false;
    }
    --cursor_;
    note(core::LogLevel::Info, "undid " + history_[cursor_].label);
    return true;
}

bool SceneEditor::redo() {
    if (!can_redo()) {
        return false;
    }
    end_gesture();
    const Command& command = history_[cursor_];
    if (!apply(command.after, "redo " + command.label).ok()) {
        return false;
    }
    note(core::LogLevel::Info, "redid " + command.label);
    ++cursor_;
    return true;
}

void SceneEditor::begin_gesture() {
    gesture_open_ = true;
}

void SceneEditor::end_gesture() {
    gesture_open_ = false;
    // Closing the gesture seals its command: a later gesture on the same target must start a new one.
    if (cursor_ > 0) {
        history_[cursor_ - 1].merge_key.clear();
    }
}

std::optional<glm::dmat4> SceneEditor::world_matrix(const core::Guid& entity) {
    const glm::dmat4* matrix = scene_.world_matrix(entity);
    if (matrix == nullptr) {
        return std::nullopt;
    }
    return *matrix;
}

bool SceneEditor::is_instance_interior(const core::Guid& entity) const {
    if (!scene_.prefab_of(entity)) {
        return false;
    }
    return find_record(scene_.to_document(), entity) == nullptr;
}

core::Result<core::Unit, scene::SceneError> SceneEditor::apply(const scene::SceneDocument& document, std::string_view what) {
    auto loaded = scene_.load(document, known_assets_);
    if (!loaded.ok()) {
        note(core::LogLevel::Error, "cannot " + std::string(what) + ": " + loaded.error().message);
        return loaded;
    }
    if (selected_ && !scene_.contains(*selected_)) {
        selected_.reset();
    }
    update_dirty();
    return loaded;
}

core::Result<core::Unit, scene::SceneError> SceneEditor::commit(const scene::SceneDocument& document, std::string_view what) {
    scene::SceneDocument before = scene_.to_document();
    auto applied = apply(document, what);
    if (applied.ok()) {
        record(what, {}, std::move(before));
    }
    return applied;
}

bool SceneEditor::merges(std::string_view merge_key) const noexcept {
    return gesture_open_ && !merge_key.empty() && cursor_ > 0 && history_[cursor_ - 1].merge_key == merge_key;
}

void SceneEditor::record(std::string_view label, std::string merge_key, scene::SceneDocument before) {
    if (merges(merge_key)) {
        history_[cursor_ - 1].after = scene_.to_document();
        return;
    }
    // A new command clears the redo steps. The merge key is kept only while a gesture is open.
    history_.resize(cursor_);
    history_.push_back(Command{std::string(label), gesture_open_ ? std::move(merge_key) : std::string{}, std::move(before),
                               scene_.to_document()});
    ++cursor_;
}

void SceneEditor::clear_history() noexcept {
    history_.clear();
    cursor_ = 0;
    gesture_open_ = false;
}

void SceneEditor::update_dirty() {
    dirty_ = scene::serialize_scene(scene_.to_document()) != saved_text_;
}

void SceneEditor::note(core::LogLevel level, std::string_view message) {
    console_->add(level, kCategory, message);
}

}  // namespace engine::editor
