// Terrain tiles as renderable meshes (US-047), on the CPU. Every tile of a mixed-LOD terrain must convert to a valid mesh, keep its splat
// channel one-to-one with its vertices, and cover its own square exactly: the projected triangle area of each tile equals the tile area,
// so a hole or an overlap would show as a mismatch. UVs run over the tile, and a square grid that is not square is rejected.

#include "engine/render/terrain_mesh.hpp"

#include "engine/render/mesh.hpp"
#include "engine/render/rt_effects.hpp"
#include "engine/world/terrain.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using engine::render::make_terrain_render_tile;
using engine::world::TerrainDesc;
using engine::world::TerrainTileMesh;
using engine::world::TileCoord;
using engine::world::build_terrain_meshes;
using engine::world::create_terrain;
using engine::world::select_terrain_lods;

namespace {

// Four 16-cell tiles in a 2 x 2 grid, with a rolling height so normals and splat vary across the tiles.
engine::world::Terrain rolling_terrain() {
    TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 2;
    desc.tile_cells = 16;
    desc.cell_size = 1.0f;
    auto created = create_terrain(desc);
    REQUIRE(created.ok());
    auto terrain = std::move(created.value());
    for (std::uint32_t z = 0; z < terrain.samples_z; ++z) {
        for (std::uint32_t x = 0; x < terrain.samples_x; ++x) {
            const std::size_t index = engine::world::sample_index(terrain, x, z);
            terrain.heights[index] = 2.0f * std::sin(0.4f * static_cast<float>(x)) + std::cos(0.3f * static_cast<float>(z));
            terrain.splat[index] = {static_cast<std::uint8_t>(x * 9 % 256), static_cast<std::uint8_t>(z * 7 % 256), 40, 80};
        }
    }
    return terrain;
}

// Twice the projected area of a triangle on the ground plane, so each triangle's contribution is an exact integer-friendly float.
float projected_area(const engine::render::MeshData& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const glm::vec3& pa = mesh.positions[a];
    const glm::vec3& pb = mesh.positions[b];
    const glm::vec3& pc = mesh.positions[c];
    const glm::vec2 ab(pb.x - pa.x, pb.z - pa.z);
    const glm::vec2 ac(pc.x - pa.x, pc.z - pa.z);
    return 0.5f * std::abs(ab.x * ac.y - ab.y * ac.x);
}

}  // namespace

TEST_CASE("Every tile of a mixed-LOD terrain converts to a valid render tile", "[unit]") {
    const auto terrain = rolling_terrain();
    // A camera at the first tile's corner gives the nearest tile LOD 0 and the far ones a coarser LOD.
    const auto lods = select_terrain_lods(terrain, glm::dvec2(0.0f, 0.0f), {8.0f, 20.0f});
    REQUIRE(lods.ok());
    const auto tiles = build_terrain_meshes(terrain, lods.value());
    REQUIRE(tiles.ok());
    REQUIRE(tiles.value().size() == 4);

    for (const TerrainTileMesh& source : tiles.value()) {
        const auto converted = make_terrain_render_tile(source);
        REQUIRE(converted.ok());
        const auto& tile = converted.value();
        CHECK(tile.tile == source.tile);
        CHECK(tile.lod == source.lod);
        CHECK(tile.mesh.positions.size() == source.vertices.size());
        CHECK(tile.splat.size() == source.vertices.size());
        CHECK(tile.mesh.indices == source.indices);
        CHECK(engine::render::validate_mesh(tile.mesh) == std::nullopt);

        // The splat channel carries the normalised weights, in the same order as the vertices.
        for (std::size_t i = 0; i < source.vertices.size(); ++i) {
            CHECK(tile.splat[i] == source.vertices[i].splat);
        }
        // UVs cover the tile corner to corner.
        CHECK(tile.mesh.uvs.front() == glm::vec2(0.0f, 0.0f));
        CHECK(tile.mesh.uvs.back() == glm::vec2(1.0f, 1.0f));
    }
}

TEST_CASE("Converted tiles cover their square exactly, with no holes or overlaps", "[unit]") {
    const auto terrain = rolling_terrain();
    // Mixed LODs on neighbours force the morphed edges, so the coverage check exercises the stitching.
    const std::vector<std::uint32_t> lods{0, 1, 2, 0};
    const auto tiles = build_terrain_meshes(terrain, lods);
    REQUIRE(tiles.ok());

    const float tile_area = 16.0f * 16.0f;  // tile_cells x tile_cells metres at 1 m per cell
    for (const TerrainTileMesh& source : tiles.value()) {
        const auto converted = make_terrain_render_tile(source);
        REQUIRE(converted.ok());
        const auto& mesh = converted.value().mesh;
        float covered = 0.0f;
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            covered += projected_area(mesh, mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]);
        }
        INFO("tile " << source.tile.x << "," << source.tile.z << " lod " << source.lod);
        CHECK(covered == Catch::Approx(tile_area).epsilon(1e-4));
    }
}

TEST_CASE("A vertex count that is not a square grid is rejected", "[unit]") {
    TerrainTileMesh bad;
    bad.vertices.resize(7);  // not a square
    bad.indices = {0, 1, 2};
    CHECK_FALSE(make_terrain_render_tile(bad).ok());
}

TEST_CASE("A pure splat weight shows its layer colour, and a blend is the weighted sum", "[unit]") {
    using engine::render::splat_colour;
    using engine::render::splat_layer_colour;
    for (std::size_t layer = 0; layer < 4; ++layer) {
        glm::vec4 weights(0.0f);
        weights[static_cast<int>(layer)] = 1.0f;
        const glm::vec3 colour = splat_colour(weights);
        CHECK(colour.x == Catch::Approx(splat_layer_colour(layer).x));
        CHECK(colour.y == Catch::Approx(splat_layer_colour(layer).y));
        CHECK(colour.z == Catch::Approx(splat_layer_colour(layer).z));
    }
    const glm::vec4 half(0.5f, 0.5f, 0.0f, 0.0f);
    const glm::vec3 mixed = splat_colour(half);
    const glm::vec3 expected = 0.5f * splat_layer_colour(0) + 0.5f * splat_layer_colour(1);
    CHECK(mixed.x == Catch::Approx(expected.x));
    CHECK(mixed.y == Catch::Approx(expected.y));
    CHECK(mixed.z == Catch::Approx(expected.z));
}

// The traced shadow of a hill on the CPU reference (US-043) over the terrain's own tiles, so the GPU suite's geometry has a checked
// baseline: the tiles of a 4 m hill under a low sun from -X must shade the ground on the hill's +X side.
TEST_CASE("The CPU traced reference shadows the ground behind a hill under a low sun", "[unit]") {
    using engine::render::RtFrame;
    using engine::render::RtInstanceRecord;
    using engine::render::RtTriangleRecord;
    using engine::render::RtMaskImage;
    using engine::render::trace_reference;

    TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 2;
    desc.tile_cells = 8;
    desc.cell_size = 1.0f;
    auto created = create_terrain(desc);
    REQUIRE(created.ok());
    auto terrain = std::move(created.value());
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            const float distance = std::hypot(static_cast<float>(ix) - 6.0f, static_cast<float>(iz) - 8.0f);
            if (distance < 3.0f) {
                terrain.heights[engine::world::sample_index(terrain, ix, iz)] = 4.0f * 0.5f * (1.0f + std::cos(glm::pi<float>() * distance / 3.0f));
            }
        }
    }
    auto tiles = build_terrain_meshes(terrain, {0, 0, 0, 0});
    REQUIRE(tiles.ok());

    std::vector<RtInstanceRecord> instances;
    std::vector<RtTriangleRecord> triangles;
    for (const auto& tile : tiles.value()) {
        auto render_tile = make_terrain_render_tile(tile);
        REQUIRE(render_tile.ok());
        const engine::render::MeshData& mesh = render_tile.value().mesh;
        RtInstanceRecord record;
        record.triangle_base = static_cast<std::uint32_t>(triangles.size());
        record.triangle_count = static_cast<std::uint32_t>(mesh.indices.size() / 3);
        record.roughness = 1.0f;
        for (std::size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
            RtTriangleRecord triangle;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                triangle.positions[corner] = mesh.positions[mesh.indices[first + corner]];
            }
            triangles.push_back(triangle);
        }
        instances.push_back(record);
    }

    // The camera of the GPU hill test: 30 m above the centre, looking straight down with a 20 degree field. The traced scene is
    // camera-relative (SceneRenderer::prepare_ray_traced), so the instances move by minus the camera and the eye is the origin.
    for (RtInstanceRecord& record : instances) {
        record.transform = glm::translate(glm::mat4(1.0f), glm::vec3(-8.0f, -30.0f, -8.0f));
    }
    RtFrame frame;
    frame.eye = glm::vec3(0.0f);
    frame.forward = glm::vec3(0.0f, -1.0f, 0.0f);
    frame.up = glm::vec3(0.0f, 0.0f, -1.0f);
    frame.fov_y_radians = glm::radians(20.0f);
    frame.aspect = 1.0f;
    frame.sun_direction = glm::normalize(glm::vec3(-1.0f, 0.6f, 0.0f));
    frame.sun_radiance = glm::vec3(1.0f);
    frame.ambient = glm::vec3(0.0f);
    frame.reflections = false;
    frame.previous_eye = frame.eye;
    frame.previous_forward = frame.forward;
    frame.previous_up = frame.up;

    const engine::render::RtReference reference = trace_reference(instances, triangles, RtMaskImage{}, frame, 64, 64);
    std::size_t hit = 0;
    std::size_t shadowed = 0;
    for (std::size_t i = 0; i < reference.shadow.size(); ++i) {
        if (reference.primary_instance[i] < 0) {
            continue;
        }
        ++hit;
        shadowed += reference.shadow[i] == 0.0f ? 1 : 0;
    }
    INFO("hit pixels " << hit << ", shadowed " << shadowed);
    CHECK(hit == reference.shadow.size());
    CHECK(shadowed >= 20);
}
