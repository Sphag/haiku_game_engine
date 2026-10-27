#include "engine/world/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace engine::world {
namespace {

// Keeps a bad description from asking for a huge grid. 2^28 samples is about 1 GiB of heights plus 256 MiB of splat weights.
constexpr std::uint64_t kMaxTerrainSamples = std::uint64_t{1} << 28;

// The attributes of one sample, shared by every tile that reads it.
struct SampleAttr {
    float height = 0.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 splat{1.0f, 0.0f, 0.0f, 0.0f};
};

SampleAttr read_attr(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) {
    return SampleAttr{sample_height(terrain, ix, iz), sample_normal(terrain, ix, iz), sample_splat(terrain, ix, iz)};
}

// Linear blend of two samples. Normals are renormalised, and splat weights stay summed to one because both ends are normalised.
SampleAttr blend(const SampleAttr& a, const SampleAttr& b, float w) {
    SampleAttr out;
    out.height = a.height + (b.height - a.height) * w;
    out.normal = glm::normalize(glm::mix(a.normal, b.normal, w));
    out.splat = glm::mix(a.splat, b.splat, w);
    return out;
}

// Per-edge stitch factor for one tile: 1 means the edge is unstitched, and 2^k means the neighbour is 2^k samples per segment coarser.
using EdgeStitch = std::array<std::uint32_t, kTileEdgeCount>;

// The attributes of a tile vertex at its own grid index (i, k). A vertex on a stitched edge that falls between two coarse vertices takes the
// blend of those coarse end samples, so it lies on the coarse edge.
SampleAttr tile_vertex_attr(const Terrain& terrain, std::uint32_t base_x, std::uint32_t base_z, std::uint32_t step, std::uint32_t n,
                            std::uint32_t i, std::uint32_t k, const EdgeStitch& stitch) {
    const std::uint32_t ix = base_x + i * step;
    const std::uint32_t iz = base_z + k * step;

    // The vertical edges (-X, +X) run along Z, so the along index is k. The horizontal edges (-Z, +Z) run along X, with along index i.
    struct EdgeCase {
        bool on_edge;
        bool along_z;
        std::uint32_t along;
        std::uint32_t segment;
    };
    const EdgeCase cases[] = {
        {i == 0, true, k, stitch[static_cast<std::size_t>(TileEdge::NegX)]},
        {i == n, true, k, stitch[static_cast<std::size_t>(TileEdge::PosX)]},
        {k == 0, false, i, stitch[static_cast<std::size_t>(TileEdge::NegZ)]},
        {k == n, false, i, stitch[static_cast<std::size_t>(TileEdge::PosZ)]},
    };
    for (const EdgeCase& edge : cases) {
        if (!edge.on_edge || edge.segment <= 1 || edge.along % edge.segment == 0) {
            continue;
        }
        const std::uint32_t start = edge.along - edge.along % edge.segment;
        const float w = static_cast<float>(edge.along - start) / static_cast<float>(edge.segment);
        const std::uint32_t fixed = edge.along_z ? ix : iz;
        const std::uint32_t base = edge.along_z ? base_z : base_x;
        auto at = [&](std::uint32_t along) {
            const std::uint32_t moving = base + along * step;
            return edge.along_z ? read_attr(terrain, fixed, moving) : read_attr(terrain, moving, fixed);
        };
        return blend(at(start), at(start + edge.segment), w);
    }
    return read_attr(terrain, ix, iz);
}

}  // namespace

core::Result<core::Unit, std::string> validate_terrain_desc(const TerrainDesc& desc) {
    if (desc.tiles_x == 0 || desc.tiles_z == 0 || desc.tiles_x > kMaxTerrainTiles || desc.tiles_z > kMaxTerrainTiles) {
        return core::failure(std::string("terrain tile counts must be between 1 and " + std::to_string(kMaxTerrainTiles)));
    }
    if (desc.tile_cells < 2 || desc.tile_cells > kMaxTileCells || (desc.tile_cells & (desc.tile_cells - 1)) != 0) {
        return core::failure(std::string("terrain tile_cells must be a power of two between 2 and " + std::to_string(kMaxTileCells)));
    }
    if (!std::isfinite(desc.cell_size) || desc.cell_size <= 0.0f) {
        return core::failure(std::string("terrain cell_size must be positive and finite"));
    }
    const std::uint64_t samples_x = std::uint64_t{desc.tiles_x} * desc.tile_cells + 1;
    const std::uint64_t samples_z = std::uint64_t{desc.tiles_z} * desc.tile_cells + 1;
    if (samples_x * samples_z > kMaxTerrainSamples) {
        return core::failure(std::string("terrain sample count exceeds the limit"));
    }
    return core::success();
}

core::Result<Terrain, std::string> create_terrain(const TerrainDesc& desc) {
    if (auto valid = validate_terrain_desc(desc); !valid.ok()) {
        return core::failure(std::string(valid.error()));
    }
    Terrain terrain;
    terrain.desc = desc;
    terrain.samples_x = desc.tiles_x * desc.tile_cells + 1;
    terrain.samples_z = desc.tiles_z * desc.tile_cells + 1;
    const std::size_t count = std::size_t{terrain.samples_x} * terrain.samples_z;
    terrain.heights.assign(count, 0.0f);
    terrain.splat.assign(count, std::array<std::uint8_t, kTerrainLayers>{255, 0, 0, 0});
    return core::success(std::move(terrain));
}

std::uint32_t max_terrain_lod(const TerrainDesc& desc) noexcept {
    std::uint32_t lod = 0;
    for (std::uint32_t cells = desc.tile_cells; cells > 1; cells >>= 1) {
        ++lod;
    }
    return lod;
}

glm::vec2 terrain_extent(const Terrain& terrain) noexcept {
    const float cell = terrain.desc.cell_size;
    return glm::vec2(static_cast<float>(terrain.samples_x - 1) * cell, static_cast<float>(terrain.samples_z - 1) * cell);
}

std::size_t sample_index(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept {
    return std::size_t{iz} * terrain.samples_x + ix;
}

float sample_height(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept {
    return terrain.heights[sample_index(terrain, ix, iz)];
}

glm::vec3 sample_normal(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept {
    const std::uint32_t left = ix > 0 ? ix - 1 : ix;
    const std::uint32_t right = ix + 1 < terrain.samples_x ? ix + 1 : ix;
    const std::uint32_t down = iz > 0 ? iz - 1 : iz;
    const std::uint32_t up = iz + 1 < terrain.samples_z ? iz + 1 : iz;
    const float dx = static_cast<float>(right - left) * terrain.desc.cell_size;
    const float dz = static_cast<float>(up - down) * terrain.desc.cell_size;
    const float slope_x = (sample_height(terrain, right, iz) - sample_height(terrain, left, iz)) / dx;
    const float slope_z = (sample_height(terrain, ix, up) - sample_height(terrain, ix, down)) / dz;
    return glm::normalize(glm::vec3(-slope_x, 1.0f, -slope_z));
}

glm::vec4 sample_splat(const Terrain& terrain, std::uint32_t ix, std::uint32_t iz) noexcept {
    const auto& raw = terrain.splat[sample_index(terrain, ix, iz)];
    const float sum = static_cast<float>(raw[0]) + raw[1] + raw[2] + raw[3];
    if (sum <= 0.0f) {
        return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    return glm::vec4(raw[0], raw[1], raw[2], raw[3]) / sum;
}

float height_at(const Terrain& terrain, glm::vec2 world_xz) noexcept {
    const float cell = terrain.desc.cell_size;
    const float fx = std::clamp(world_xz.x / cell, 0.0f, static_cast<float>(terrain.samples_x - 1));
    const float fz = std::clamp(world_xz.y / cell, 0.0f, static_cast<float>(terrain.samples_z - 1));
    const std::uint32_t ix = std::min(static_cast<std::uint32_t>(fx), terrain.samples_x - 2);
    const std::uint32_t iz = std::min(static_cast<std::uint32_t>(fz), terrain.samples_z - 2);
    const float tx = fx - static_cast<float>(ix);
    const float tz = fz - static_cast<float>(iz);
    const float low = sample_height(terrain, ix, iz) + (sample_height(terrain, ix + 1, iz) - sample_height(terrain, ix, iz)) * tx;
    const float high =
        sample_height(terrain, ix, iz + 1) + (sample_height(terrain, ix + 1, iz + 1) - sample_height(terrain, ix, iz + 1)) * tx;
    return low + (high - low) * tz;
}

float tile_distance(const Terrain& terrain, TileCoord tile, glm::dvec2 camera_xz) noexcept {
    // The distance is measured in double from the camera's exact position; only the result is single precision.
    const double size = static_cast<double>(terrain.desc.tile_cells) * static_cast<double>(terrain.desc.cell_size);
    const glm::dvec2 min = glm::dvec2(tile.x, tile.z) * size;
    const glm::dvec2 max = min + glm::dvec2(size);
    const glm::dvec2 nearest = glm::clamp(camera_xz, min, max);
    return static_cast<float>(glm::length(camera_xz - nearest));
}

std::uint32_t select_tile_lod(float distance, const std::vector<float>& ranges, std::uint32_t max_lod) {
    std::uint32_t lod = 0;
    for (float range : ranges) {
        if (distance >= range) {
            ++lod;
        }
    }
    return std::min(lod, max_lod);
}

core::Result<std::vector<std::uint32_t>, std::string> select_terrain_lods(const Terrain& terrain, glm::dvec2 camera_xz,
                                                                         const std::vector<float>& ranges) {
    if (!std::isfinite(camera_xz.x) || !std::isfinite(camera_xz.y)) {
        return core::failure(std::string("camera position must be finite"));
    }
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (!std::isfinite(ranges[i]) || ranges[i] < 0.0f || (i > 0 && ranges[i] < ranges[i - 1])) {
            return core::failure(std::string("LOD ranges must be finite, non-negative, and non-decreasing"));
        }
    }
    const std::uint32_t max_lod = max_terrain_lod(terrain.desc);
    std::vector<std::uint32_t> lods;
    lods.reserve(std::size_t{terrain.desc.tiles_x} * terrain.desc.tiles_z);
    for (std::uint32_t z = 0; z < terrain.desc.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < terrain.desc.tiles_x; ++x) {
            lods.push_back(select_tile_lod(tile_distance(terrain, TileCoord{x, z}, camera_xz), ranges, max_lod));
        }
    }
    return core::success(std::move(lods));
}

core::Result<TerrainTileMesh, std::string> build_tile_mesh(const Terrain& terrain, TileCoord tile, std::uint32_t lod,
                                                           const std::array<std::uint32_t, kTileEdgeCount>& neighbour_lods) {
    if (tile.x >= terrain.desc.tiles_x || tile.z >= terrain.desc.tiles_z) {
        return core::failure(std::string("terrain tile is outside the terrain"));
    }
    const std::uint32_t max_lod = max_terrain_lod(terrain.desc);
    if (lod > max_lod) {
        return core::failure(std::string("terrain tile LOD exceeds the tile's maximum"));
    }
    for (std::uint32_t neighbour : neighbour_lods) {
        if (neighbour > max_lod) {
            return core::failure(std::string("terrain neighbour LOD exceeds the tile's maximum"));
        }
    }

    // A neighbour that is coarser stitches its shared edge with 2^(neighbour - lod)-sample segments. The terrain's border is never stitched.
    EdgeStitch stitch{1, 1, 1, 1};
    const bool has_neg_x = tile.x > 0;
    const bool has_pos_x = tile.x + 1 < terrain.desc.tiles_x;
    const bool has_neg_z = tile.z > 0;
    const bool has_pos_z = tile.z + 1 < terrain.desc.tiles_z;
    const bool inside[kTileEdgeCount] = {has_neg_x, has_pos_x, has_neg_z, has_pos_z};
    for (std::size_t e = 0; e < kTileEdgeCount; ++e) {
        if (inside[e] && neighbour_lods[e] > lod) {
            stitch[e] = std::uint32_t{1} << (neighbour_lods[e] - lod);
        }
    }

    TerrainTileMesh mesh;
    mesh.tile = tile;
    mesh.lod = lod;
    const std::uint32_t step = std::uint32_t{1} << lod;
    const std::uint32_t n = terrain.desc.tile_cells >> lod;
    const std::uint32_t base_x = tile.x * terrain.desc.tile_cells;
    const std::uint32_t base_z = tile.z * terrain.desc.tile_cells;
    const float cell = terrain.desc.cell_size;

    mesh.vertices.reserve(std::size_t{n + 1} * (n + 1));
    for (std::uint32_t k = 0; k <= n; ++k) {
        for (std::uint32_t i = 0; i <= n; ++i) {
            const SampleAttr attr = tile_vertex_attr(terrain, base_x, base_z, step, n, i, k, stitch);
            TerrainVertex vertex;
            vertex.position = glm::vec3(static_cast<float>(base_x + i * step) * cell, attr.height,
                                        static_cast<float>(base_z + k * step) * cell);
            vertex.normal = attr.normal;
            vertex.splat = attr.splat;
            mesh.vertices.push_back(vertex);
        }
    }

    // Each cell gives two triangles, counter-clockwise when seen from +Y.
    const std::uint32_t stride = n + 1;
    mesh.indices.reserve(std::size_t{n} * n * 6);
    for (std::uint32_t k = 0; k < n; ++k) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t v00 = k * stride + i;
            const std::uint32_t v10 = v00 + 1;
            const std::uint32_t v01 = v00 + stride;
            const std::uint32_t v11 = v01 + 1;
            mesh.indices.insert(mesh.indices.end(), {v00, v01, v11, v00, v11, v10});
        }
    }
    return core::success(std::move(mesh));
}

std::uint32_t neighbour_lod(const Terrain& terrain, const std::vector<std::uint32_t>& tile_lods, TileCoord tile, TileEdge edge) {
    const std::size_t self = std::size_t{tile.z} * terrain.desc.tiles_x + tile.x;
    std::uint32_t nx = tile.x;
    std::uint32_t nz = tile.z;
    switch (edge) {
        case TileEdge::NegX:
            if (tile.x == 0) {
                return tile_lods[self];
            }
            nx = tile.x - 1;
            break;
        case TileEdge::PosX:
            if (tile.x + 1 >= terrain.desc.tiles_x) {
                return tile_lods[self];
            }
            nx = tile.x + 1;
            break;
        case TileEdge::NegZ:
            if (tile.z == 0) {
                return tile_lods[self];
            }
            nz = tile.z - 1;
            break;
        case TileEdge::PosZ:
            if (tile.z + 1 >= terrain.desc.tiles_z) {
                return tile_lods[self];
            }
            nz = tile.z + 1;
            break;
    }
    return tile_lods[std::size_t{nz} * terrain.desc.tiles_x + nx];
}

core::Result<std::vector<TerrainTileMesh>, std::string> build_terrain_meshes(const Terrain& terrain,
                                                                             const std::vector<std::uint32_t>& tile_lods) {
    const std::size_t tile_count = std::size_t{terrain.desc.tiles_x} * terrain.desc.tiles_z;
    if (tile_lods.size() != tile_count) {
        return core::failure(std::string("terrain LOD list must hold one entry per tile"));
    }
    std::vector<TerrainTileMesh> meshes;
    meshes.reserve(tile_count);
    for (std::uint32_t z = 0; z < terrain.desc.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < terrain.desc.tiles_x; ++x) {
            const TileCoord tile{x, z};
            const std::array<std::uint32_t, kTileEdgeCount> neighbours{
                neighbour_lod(terrain, tile_lods, tile, TileEdge::NegX),
                neighbour_lod(terrain, tile_lods, tile, TileEdge::PosX),
                neighbour_lod(terrain, tile_lods, tile, TileEdge::NegZ),
                neighbour_lod(terrain, tile_lods, tile, TileEdge::PosZ),
            };
            auto mesh = build_tile_mesh(terrain, tile, tile_lods[std::size_t{z} * terrain.desc.tiles_x + x], neighbours);
            if (!mesh.ok()) {
                return core::failure(std::string(mesh.error()));
            }
            meshes.push_back(std::move(mesh.value()));
        }
    }
    return core::success(std::move(meshes));
}

}  // namespace engine::world
