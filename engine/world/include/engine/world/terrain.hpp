#pragma once

// Heightfield terrain runtime (US-047): the CPU side of the tiled landscape. The terrain is one global grid of height samples. A tile
// covers tile_cells x tile_cells cells, so its edges hold tile_cells + 1 samples. Neighbouring tiles read the same stored samples on their
// shared edge, so tile boundaries agree by construction; the authoring contract is that a sample is edited once, in the global grid.
//
// A tile renders at one LOD L. Its grid is (tile_cells >> L) cells per side, and each cell is 2^L samples wide. Where a neighbour is at a
// coarser LOD, the shared edge is stitched: the fine tile keeps its vertices but morphs the ones between coarse vertices onto the coarse
// edge (height, normal and splat weights are interpolated from the two coarse end samples). The coarse tile then has no vertex on that
// edge that the fine tile lacks, and the two surfaces meet along the same line. Coverage is exact in 2D, so the fixture can check for holes
// and overlaps by point sampling. Stitching keeps T-junctions, so rasterized cracks are not excluded analytically; no skirts are generated.
//
// Splat blending stores four material weights per sample (RGBA8). Weights are normalised at read time, so an empty sample blends to zero.
// Everything here is CPU-only and deterministic. The renderer, shadows, depth, and ray-traced geometry consume the meshes it returns.

#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::world {

inline constexpr std::uint32_t kTerrainLayers = 4;
inline constexpr std::uint32_t kMaxTerrainTiles = 1024;
inline constexpr std::uint32_t kMaxTileCells = 1024;

// The shared-world contract (PRD): 32 x 32 tiles of 256 cells at 1 m, so each tile has 257 x 257 samples.
struct TerrainDesc {
    std::uint32_t tiles_x = 1;
    std::uint32_t tiles_z = 1;
    std::uint32_t tile_cells = 256;  // Power of two, 2..kMaxTileCells.
    float cell_size = 1.0f;          // Metres between neighbouring samples.
};

// Tile index on the tile grid. The tile at (x, z) covers world X in [x, x+1) * tile_size and Z likewise, measured from the origin.
struct TileCoord {
    std::uint32_t x = 0;
    std::uint32_t z = 0;

    friend bool operator==(const TileCoord& a, const TileCoord& b) {
        return a.x == b.x && a.z == b.z;
    }
};

// Side indices for per-edge data: the tile's -X, +X, -Z, and +Z edges.
enum class TileEdge : std::uint8_t { NegX = 0, PosX = 1, NegZ = 2, PosZ = 3 };
inline constexpr std::size_t kTileEdgeCount = 4;

struct Terrain {
    TerrainDesc desc;
    std::uint32_t samples_x = 0;  // tiles_x * tile_cells + 1
    std::uint32_t samples_z = 0;  // tiles_z * tile_cells + 1
    std::vector<float> heights;   // samples_x * samples_z, row-major in Z, then X.
    std::vector<std::array<std::uint8_t, kTerrainLayers>> splat;  // Same layout; raw RGBA8 layer weights.
};

// One vertex of a tile mesh. The splat values are normalised (they sum to one) and index the four layers in order.
struct TerrainVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 splat{1.0f, 0.0f, 0.0f, 0.0f};
};

struct TerrainTileMesh {
    TileCoord tile;
    std::uint32_t lod = 0;
    std::vector<TerrainVertex> vertices;  // (cells + 1)^2, row-major in Z, then X.
    std::vector<std::uint32_t> indices;   // Two counter-clockwise (seen from +Y) triangles per cell.
};

// Validates the description: counts within the limits, tile_cells a power of two, and a positive cell size.
[[nodiscard]] core::Result<core::Unit, std::string> validate_terrain_desc(const TerrainDesc& desc);

// Creates a flat terrain at height zero with every sample carrying layer 0.
[[nodiscard]] core::Result<Terrain, std::string> create_terrain(const TerrainDesc& desc);

[[nodiscard]] std::uint32_t max_terrain_lod(const TerrainDesc& desc) noexcept;
[[nodiscard]] glm::vec2 terrain_extent(const Terrain& terrain) noexcept;  // World size in metres (X, Z).

[[nodiscard]] std::size_t sample_index(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept;
[[nodiscard]] float sample_height(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept;

// Central-difference normal at a sample. Samples on the terrain border clamp to their neighbour inside the grid.
[[nodiscard]] glm::vec3 sample_normal(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept;

// Normalised layer weights at a sample. An all-zero sample reads as layer 0.
[[nodiscard]] glm::vec4 sample_splat(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept;

// Height at a world position, bilinearly interpolated between samples. Positions outside the terrain clamp to its edge.
[[nodiscard]] float height_at(const Terrain& terrain, glm::vec2 world_xz) noexcept;

// Distance from a camera position to a tile's nearest point, in metres on the ground plane.
[[nodiscard]] float tile_distance(const Terrain& terrain, TileCoord tile, glm::dvec2 camera_xz) noexcept;

// LOD by distance. `ranges[i]` is the distance where LOD i + 1 begins, so the result is the count of ranges at or below the distance.
// The ranges must be non-decreasing; the result is capped at max_terrain_lod.
[[nodiscard]] std::uint32_t select_tile_lod(float distance, const std::vector<float>& ranges, std::uint32_t max_lod);

// Selects a LOD for every tile in row-major order (Z, then X). `ranges` has the same meaning as in select_tile_lod.
[[nodiscard]] core::Result<std::vector<std::uint32_t>, std::string> select_terrain_lods(const Terrain& terrain, glm::dvec2 camera_xz,
                                                                                       const std::vector<float>& ranges);

// Builds one tile's mesh at `lod`. `neighbour_lods` holds the LOD of the tile across each edge (indexed by TileEdge). An edge whose
// neighbour is finer, or at the same LOD, is not stitched; an edge on the terrain's border is ignored. A neighbour's LOD of
// lod + k (k >= 1) stitches the edge with 2^k-sample segments. Errors on a bad tile, LOD, or neighbour LOD.
[[nodiscard]] core::Result<TerrainTileMesh, std::string> build_tile_mesh(const Terrain& terrain, TileCoord tile, std::uint32_t lod,
                                                                         const std::array<std::uint32_t, kTileEdgeCount>& neighbour_lods);

// Builds the meshes for every tile at its LOD from select_terrain_lods, reading each tile's neighbours from the same LOD list.
[[nodiscard]] core::Result<std::vector<TerrainTileMesh>, std::string> build_terrain_meshes(const Terrain& terrain,
                                                                                           const std::vector<std::uint32_t>& tile_lods);

// Neighbour LOD for an edge of a tile. The terrain's border returns the tile's own LOD, so that edge is never stitched.
[[nodiscard]] std::uint32_t neighbour_lod(const Terrain& terrain, const std::vector<std::uint32_t>& tile_lods, TileCoord tile,
                                          TileEdge edge);

}  // namespace engine::world
