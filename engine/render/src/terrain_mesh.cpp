#include "engine/render/terrain_mesh.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render {

glm::vec3 splat_layer_colour(std::size_t layer) noexcept {
    // Grass, rock, sand, and snow, in the order of the four splat weights.
    static constexpr glm::vec3 kLayerColours[4] = {
        {0.25f, 0.45f, 0.15f},
        {0.45f, 0.42f, 0.40f},
        {0.78f, 0.70f, 0.50f},
        {0.92f, 0.94f, 0.96f},
    };
    return kLayerColours[layer < 4 ? layer : 3];
}

glm::vec3 splat_colour(const glm::vec4& weights) noexcept {
    return weights.x * splat_layer_colour(0) + weights.y * splat_layer_colour(1) + weights.z * splat_layer_colour(2) +
           weights.w * splat_layer_colour(3);
}

core::Result<TerrainRenderTile, std::string> make_terrain_render_tile(const world::TerrainTileMesh& tile) {
    // The vertices form a square grid of (cells + 1) x (cells + 1), row-major in Z, then X. Recover the side from the count.
    const std::size_t count = tile.vertices.size();
    const auto side = static_cast<std::size_t>(std::llround(std::sqrt(static_cast<double>(count))));
    if (side < 2 || side * side != count) {
        return core::failure(std::string("terrain tile vertex count is not a square grid"));
    }
    const std::size_t cells = side - 1;

    TerrainRenderTile out;
    out.tile = tile.tile;
    out.lod = tile.lod;
    out.mesh.positions.reserve(count);
    out.mesh.normals.reserve(count);
    out.mesh.uvs.reserve(count);
    out.splat.reserve(count);
    const float inv_cells = 1.0f / static_cast<float>(cells);
    for (std::size_t i = 0; i < count; ++i) {
        const world::TerrainVertex& v = tile.vertices[i];
        const std::size_t column = i % side;
        const std::size_t row = i / side;
        out.mesh.positions.push_back(v.position);
        out.mesh.normals.push_back(v.normal);
        out.mesh.uvs.emplace_back(static_cast<float>(column) * inv_cells, static_cast<float>(row) * inv_cells);
        out.splat.push_back(v.splat);
    }
    out.mesh.indices = tile.indices;

    if (const std::optional<std::string> problem = validate_mesh(out.mesh)) {
        return core::failure(std::string("terrain tile mesh is invalid: " + *problem));
    }
    return core::success(std::move(out));
}

}  // namespace engine::render
