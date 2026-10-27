#pragma once

// Terrain tiles as renderable meshes (US-047). A tile from engine/world is a heightfield at one LOD with world-space positions, normals,
// and indices. This bridge turns it into a MeshData that the scene renderer uploads like any static mesh, and keeps the four-layer splat
// weights alongside it as a per-vertex channel. The splat is not a MeshData field: the mesh path has no fifth attribute yet, so a terrain
// shader reads the channel from `splat` when that path is added. Tile UVs run from (0, 0) at the tile's -X, -Z corner to (1, 1) at its
// +X, +Z corner, so a material can sample a per-tile texture over the whole tile.

#include "engine/render/mesh.hpp"
#include "engine/world/terrain.hpp"

#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace engine::render {

struct TerrainRenderTile {
    world::TileCoord tile;
    std::uint32_t lod = 0;
    MeshData mesh;                 // object space equals world space; the tile is drawn with an identity transform
    std::vector<glm::vec4> splat;  // one normalised four-layer weight vector per vertex, same order as mesh.positions
};

// The constant colour of each splat layer (layer 0 to 3), the first cut of the terrain material (option b in the US-047 notes). The GPU
// terrain shader must blend with the same values, so a fixture can compare its output with splat_colour.
[[nodiscard]] glm::vec3 splat_layer_colour(std::size_t layer) noexcept;

// The weighted sum of the four layer colours for one normalised splat vector.
[[nodiscard]] glm::vec3 splat_colour(const glm::vec4& weights) noexcept;

// Converts a tile mesh. Errors when the vertex count is not a square grid, the splat channel does not match, or the mesh fails
// validate_mesh.
[[nodiscard]] core::Result<TerrainRenderTile, std::string> make_terrain_render_tile(const world::TerrainTileMesh& tile);

}  // namespace engine::render
