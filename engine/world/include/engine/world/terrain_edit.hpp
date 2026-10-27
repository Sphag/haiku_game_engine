#pragma once

// Terrain sculpting and painting (US-071), on the CPU. A brush stamp changes samples of the global grid in `Terrain`, so a sample on a tile
// edge is one value that both tiles read: the edit reconciles heights and normals across tile boundaries by construction. The document
// records which tiles a stamp touched, so the LOD meshes, the RT geometry, and the collision heightfields of exactly those tiles are
// rebuilt; each tile carries a revision that a consumer compares to know its copy is stale.
//
// A stroke is a run of stamps between begin_stroke and end_stroke. It is one undo command: the document keeps each touched sample's value
// before the stroke (captured at its first touch) and after it, so undo restores the "before" values and redo the "after" values. A
// stroke that changes nothing adds no command. The file format stores the grid exactly (float bit patterns and raw splat bytes), so
// save, reopen, and cook produce the same bytes.

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/world/terrain.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::world {

enum class BrushKind : std::uint8_t { Raise, Lower, Smooth, Flatten, Paint };

struct Brush {
    BrushKind kind = BrushKind::Raise;
    float radius = 4.0f;        // Metres on the ground plane; must be positive.
    float strength = 0.5f;      // (0, 1]. Scales each stamp at the brush centre.
    float falloff = 0.5f;      // [0, 1]. The fraction of the radius over which the weight eases from one to zero; zero is a hard edge.
    std::uint32_t layer = 0;    // Paint only: the splat layer to add, below kTerrainLayers.
    float target_height = 0.0f; // Flatten only: the height that the brush pulls toward, in metres.
};

// Height change per stamp at full strength and weight, in metres for Raise and Lower.
inline constexpr float kBrushHeightStep = 0.25f;

[[nodiscard]] core::Result<core::Unit, std::string> validate_brush(const Brush& brush);

// Weight of a sample at `distance_over_radius` (its distance divided by the radius). One inside the solid core, zero at and beyond the
// radius, and smoothstep across the falloff band in between.
[[nodiscard]] float brush_weight(float distance_over_radius, float falloff) noexcept;

// Sample rectangle with exclusive upper bounds. Empty when x0 >= x1 or z0 >= z1.
struct SampleSpan {
    std::uint32_t x0 = 0;
    std::uint32_t z0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t z1 = 0;

    [[nodiscard]] bool empty() const noexcept {
        return x0 >= x1 || z0 >= z1;
    }
};

// The samples a stamp at `centre_world` (metres, X and Z) can change, clamped to the grid.
[[nodiscard]] SampleSpan brush_span(const Terrain& terrain, const Brush& brush, glm::vec2 centre_world) noexcept;

// Tiles that own at least one sample of `span`. A sample on a tile edge belongs to both tiles, so an edit on a shared edge reports both.
[[nodiscard]] std::vector<TileCoord> tiles_touching(const Terrain& terrain, const SampleSpan& span);

// Applies one stamp. Fails for an invalid brush. Returns the span it considered (empty when the stamp misses the grid).
[[nodiscard]] core::Result<SampleSpan, std::string> apply_brush_stamp(Terrain& terrain, const Brush& brush, glm::vec2 centre_world);

// Edit history and dirty tracking over one terrain. Every change goes through a stroke, so the document is valid after each call.
class TerrainDocument {
public:
    [[nodiscard]] static core::Result<TerrainDocument, std::string> create(const TerrainDesc& desc);
    [[nodiscard]] static TerrainDocument from_terrain(Terrain terrain);

    [[nodiscard]] const Terrain& terrain() const noexcept {
        return terrain_;
    }

    // Starts a stroke. Fails for an invalid brush or when a stroke is already open.
    [[nodiscard]] core::Result<core::Unit, std::string> begin_stroke(const Brush& brush);
    // Applies one stamp to the open stroke. Fails when no stroke is open.
    [[nodiscard]] core::Result<core::Unit, std::string> stamp(glm::vec2 centre_world);
    // Closes the stroke as one undo command. Returns false when it changed nothing (no command is added). Fails when no stroke is open.
    [[nodiscard]] core::Result<bool, std::string> end_stroke();
    // Closes the stroke and restores every sample it touched. Fails when no stroke is open.
    [[nodiscard]] core::Result<core::Unit, std::string> cancel_stroke();
    [[nodiscard]] bool stroke_open() const noexcept {
        return stroke_.has_value();
    }

    // Undo and redo return false when there is nothing to undo or redo, or while a stroke is open.
    bool undo();
    bool redo();
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;

    // Tiles changed since the last call, sorted row-major (Z, then X). The caller rebuilds their LOD meshes, RT geometry, and colliders.
    [[nodiscard]] std::vector<TileCoord> take_dirty_tiles();
    // Increases on every change to the tile, including undo and redo. Zero for a tile never touched.
    [[nodiscard]] std::uint64_t tile_revision(TileCoord tile) const;

    // True when the terrain differs from the last saved (or opened) state.
    [[nodiscard]] bool dirty() const noexcept;
    void mark_saved() noexcept;

private:
    struct SampleChange {
        std::size_t index = 0;
        float height_before = 0.0f;
        float height_after = 0.0f;
        std::array<std::uint8_t, kTerrainLayers> splat_before{};
        std::array<std::uint8_t, kTerrainLayers> splat_after{};
    };
    struct Command {
        std::vector<SampleChange> changes;
    };
    struct Stroke {
        Brush brush;
        std::unordered_map<std::size_t, SampleChange> touched;
    };

    explicit TerrainDocument(Terrain terrain);
    void mark_samples(const std::vector<std::size_t>& indices);
    void apply_command(const Command& command, bool use_after);

    Terrain terrain_;
    std::vector<Command> history_;
    std::size_t history_pos_ = 0;           // Commands before this index are applied.
    std::size_t saved_pos_ = 0;             // history_pos_ at the last save; npos when the history moved past it.
    std::optional<Stroke> stroke_;
    std::vector<TileCoord> dirty_;
    std::vector<std::uint64_t> revisions_;  // One per tile, row-major.
};

// Terrain file format (.terrain): magic "OWTR", version 1, the description, the sample counts, then heights as IEEE bit patterns and
// splat bytes, all little-endian. The loader checks the size, the description, and that every height is finite.
[[nodiscard]] std::string serialize_terrain(const Terrain& terrain);
[[nodiscard]] core::Result<Terrain, std::string> parse_terrain(std::string_view bytes);

[[nodiscard]] core::Result<core::Unit, std::string> save_terrain(const core::Path& path, const Terrain& terrain);
[[nodiscard]] core::Result<Terrain, std::string> load_terrain(const core::Path& path);

}  // namespace engine::world
