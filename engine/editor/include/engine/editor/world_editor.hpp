#pragma once

// World partition editing session (US-072), with no ImGui or device dependency. It owns one authored world.World in memory, which is the
// only copy of the edits: a cell that is unloaded or suppressed keeps its objects and its dirty flag here, so no residency change can
// lose an edit. Save writes the dirty cells to the world directory. Residency is a StreamingManager over a derived stream cache
// (`<directory>/.stream`), which holds the current contents of every cell; the cache is rebuilt from the World before the next residency
// update after an edit, so streaming never reads a stale cell. The editor camera drives residency. It is a plain position, not the
// gameplay camera, and this session holds no script engine, so residency changes never run a gameplay script (Play does that).

#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace engine::editor {

class WorldEditor {
public:
    // A new world with the given grid. Nothing is written until save(), and every cell it creates is dirty.
    [[nodiscard]] static core::Result<WorldEditor, std::string> create(const world::WorldDesc& desc, const core::Path& directory);
    // Opens a world directory: world.json, layer.world, and every *.cell file. The document starts clean.
    [[nodiscard]] static core::Result<WorldEditor, std::string> open(const core::Path& directory);

    // Movable, not assignable: the streaming manager refers to the job system by address, and assignment would destroy one under the other.
    WorldEditor(WorldEditor&&) noexcept;
    WorldEditor& operator=(WorldEditor&&) = delete;
    WorldEditor(const WorldEditor&) = delete;
    WorldEditor& operator=(const WorldEditor&) = delete;
    ~WorldEditor();

    [[nodiscard]] const world::World& world() const noexcept {
        return world_;
    }
    [[nodiscard]] const core::Path& directory() const noexcept {
        return directory_;
    }
    // True when any cell or the layer has changes that save() has not written.
    [[nodiscard]] bool dirty() const noexcept;

    // Adds an object to the layer, or to the cell that owns its position. Creates that cell if it does not exist yet.
    [[nodiscard]] core::Result<core::Unit, std::string> add_object(world::WorldObject object, bool always_loaded);
    // Moves an object. Its GUID, name, and references are kept; a move between cells dirties both cells.
    [[nodiscard]] core::Result<core::Unit, std::string> move_object(const core::Guid& id, glm::vec3 position);

    // Writes the dirty cells and layer to the world directory, and writes world.json when it is missing. Returns the number of
    // files written. A failed write leaves the failed cell dirty.
    [[nodiscard]] core::Result<std::size_t, std::string> save();

    // Residency controls. Load makes a cell resident and clears a suppression. Pin keeps a cell resident outside the prefetch ring.
    // Unload drops the streamed copy, even inside the active window, and clears the pin. None of them touch authored data.
    void load_cell(world::CellCoord coord);
    void set_pinned(world::CellCoord coord, bool pinned);
    void unload_cell(world::CellCoord coord);
    [[nodiscard]] bool pinned(world::CellCoord coord) const;
    [[nodiscard]] bool suppressed(world::CellCoord coord) const;

    // The editor camera's XZ position, which drives residency. Navigating to a cell puts it at the cell's centre.
    void set_camera(glm::dvec2 xz) noexcept;
    [[nodiscard]] glm::dvec2 camera() const noexcept {
        return camera_;
    }
    void go_to_cell(world::CellCoord coord);

    // Advances residency between frames: rebuilds the stream cache when an edit made it stale, updates the camera's window, and applies
    // the safe point. Call once per editor frame. A failed cache write is returned, and the cache is retried on the next call.
    [[nodiscard]] core::Result<core::Unit, std::string> tick();
    [[nodiscard]] world::CellPhase phase(world::CellCoord coord) const;
    [[nodiscard]] const world::StreamingStats& stats() const;
    [[nodiscard]] const world::StreamingBudget& budget() const noexcept {
        return budget_;
    }
    [[nodiscard]] const world::StreamingWindow& window() const noexcept {
        return window_;
    }

private:
    WorldEditor(world::World world, core::Path directory);

    // Writes every cell to the stream cache, then rebuilds the streaming manager over it. Pins and suppressions carry over.
    [[nodiscard]] core::Result<core::Unit, std::string> rebuild_stream();
    // Replaces the streaming manager without touching the cache. The old manager is destroyed first, so its jobs finish before a file changes.
    void reset_stream();

    world::World world_;
    core::Path stream_directory_;  // the derived stream cache, inside directory_
    core::Path directory_;
    world::StreamingWindow window_;
    world::StreamingBudget budget_;
    glm::dvec2 camera_{0.0, 0.0};
    bool stream_stale_ = true;
    // Declared in this order: the streaming manager must die before the jobs it submits to.
    std::unique_ptr<core::JobSystem> jobs_;
    std::unique_ptr<world::StreamingManager> stream_;
};

}  // namespace engine::editor
