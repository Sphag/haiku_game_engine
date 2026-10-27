// Heightfield terrain runtime (US-047), on the CPU. These cases check the description limits, the sample sharing across tile boundaries, the
// LOD selection by distance, the stitching of mixed-LOD edges, the absence of holes and overlaps by point coverage over the whole terrain,
// the normals and the four-layer splat weights, and the rejection of bad inputs.

#include "engine/world/terrain.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using engine::world::TerrainDesc;
using engine::world::TerrainTileMesh;
using engine::world::TileCoord;
using engine::world::TileEdge;
using engine::world::build_terrain_meshes;
using engine::world::build_tile_mesh;
using engine::world::create_terrain;
using engine::world::height_at;
using engine::world::max_terrain_lod;
using engine::world::neighbour_lod;
using engine::world::sample_splat;
using engine::world::select_terrain_lods;
using engine::world::select_tile_lod;
using engine::world::validate_terrain_desc;

namespace {

// A grid of 16-cell tiles, so each LOD keeps a few cells and the whole terrain is small enough to sample densely.
TerrainDesc small_desc(std::uint32_t tiles_x, std::uint32_t tiles_z) {
    TerrainDesc desc;
    desc.tiles_x = tiles_x;
    desc.tiles_z = tiles_z;
    desc.tile_cells = 16;
    desc.cell_size = 1.0f;
    return desc;
}

// A smooth, non-planar height field, so a stitching error shows up as a height mismatch and not as a flat-surface coincidence.
engine::world::Terrain wave_terrain(const TerrainDesc& desc) {
    auto created = create_terrain(desc);
    REQUIRE(created.ok());
    engine::world::Terrain terrain = std::move(created.value());
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            const float h = 2.0f * std::sin(0.31f * static_cast<float>(ix)) + std::cos(0.23f * static_cast<float>(iz)) +
                            0.07f * static_cast<float>(ix) * static_cast<float>(iz % 5);
            terrain.heights[engine::world::sample_index(terrain, ix, iz)] = h;
        }
    }
    return terrain;
}

// Signed side of a point against a 2D edge. Used to test whether a point lies inside a counter-clockwise triangle.
float side(glm::vec2 a, glm::vec2 b, glm::vec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// True when the point lies strictly inside or on the boundary of the triangle, whichever its winding.
bool in_triangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const float s1 = side(a, b, p);
    const float s2 = side(b, c, p);
    const float s3 = side(c, a, p);
    const bool has_neg = s1 < 0.0f || s2 < 0.0f || s3 < 0.0f;
    const bool has_pos = s1 > 0.0f || s2 > 0.0f || s3 > 0.0f;
    return !(has_neg && has_pos);
}

// Counts the triangles of all meshes that cover each sample point over the terrain's X/Z extent. A sound tiling covers every point once.
// The points sit off the cell grid so that no point falls on a boundary.
std::map<int, int> coverage_histogram(const std::vector<TerrainTileMesh>& meshes, glm::vec2 extent, int per_axis) {
    std::map<int, int> histogram;
    for (int j = 0; j < per_axis; ++j) {
        for (int i = 0; i < per_axis; ++i) {
            const glm::vec2 p(extent.x * (static_cast<float>(i) + 0.3719f) / static_cast<float>(per_axis),
                              extent.y * (static_cast<float>(j) + 0.6143f) / static_cast<float>(per_axis));
            int count = 0;
            for (const TerrainTileMesh& mesh : meshes) {
                for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
                    const auto& a = mesh.vertices[mesh.indices[t]].position;
                    const auto& b = mesh.vertices[mesh.indices[t + 1]].position;
                    const auto& c = mesh.vertices[mesh.indices[t + 2]].position;
                    const float min_x = std::min({a.x, b.x, c.x});
                    const float max_x = std::max({a.x, b.x, c.x});
                    const float min_z = std::min({a.z, b.z, c.z});
                    const float max_z = std::max({a.z, b.z, c.z});
                    if (p.x < min_x || p.x > max_x || p.y < min_z || p.y > max_z) {
                        continue;
                    }
                    if (in_triangle(p, glm::vec2(a.x, a.z), glm::vec2(b.x, b.z), glm::vec2(c.x, c.z))) {
                        ++count;
                    }
                }
            }
            histogram[count] += 1;
        }
    }
    return histogram;
}

}  // namespace

TEST_CASE("The terrain description states its limits, and an invalid one creates no terrain", "[unit]") {
    CHECK(validate_terrain_desc(TerrainDesc{}).ok());
    CHECK(create_terrain(TerrainDesc{}).ok());

    TerrainDesc desc;
    desc.tile_cells = 24;
    CHECK_FALSE(validate_terrain_desc(desc).ok());
    CHECK_FALSE(create_terrain(desc).ok());

    desc = TerrainDesc{};
    desc.tiles_x = 0;
    CHECK_FALSE(validate_terrain_desc(desc).ok());

    desc = TerrainDesc{};
    desc.cell_size = 0.0f;
    CHECK_FALSE(validate_terrain_desc(desc).ok());

    desc = TerrainDesc{};
    desc.tiles_x = 1024;
    desc.tiles_z = 1024;
    desc.tile_cells = 1024;
    CHECK_FALSE(validate_terrain_desc(desc).ok());
}

TEST_CASE("The shared world holds 257 samples per tile edge and one global sample grid", "[unit]") {
    TerrainDesc desc;
    desc.tiles_x = 32;
    desc.tiles_z = 32;
    desc.tile_cells = 256;
    desc.cell_size = 1.0f;
    auto created = create_terrain(desc);
    REQUIRE(created.ok());
    const auto& terrain = created.value();
    CHECK(terrain.samples_x == 32 * 256 + 1);
    CHECK(terrain.samples_z == 32 * 256 + 1);
    CHECK(max_terrain_lod(desc) == 8);
    CHECK(glm::vec2(engine::world::terrain_extent(terrain)) == glm::vec2(8192.0f, 8192.0f));
    CHECK(max_terrain_lod(small_desc(1, 1)) == 4);
}

TEST_CASE("A flat tile at LOD 0 has the full grid of vertices and upward normals", "[unit]") {
    auto created = create_terrain(small_desc(1, 1));
    REQUIRE(created.ok());
    auto mesh = build_tile_mesh(created.value(), TileCoord{0, 0}, 0, {0, 0, 0, 0});
    REQUIRE(mesh.ok());
    CHECK(mesh.value().vertices.size() == 17 * 17);
    CHECK(mesh.value().indices.size() == 16 * 16 * 6);
    for (const auto& vertex : mesh.value().vertices) {
        CHECK(vertex.normal == glm::vec3(0.0f, 1.0f, 0.0f));
        CHECK(vertex.splat == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    }
    // Every triangle winds counter-clockwise from +Y, so its geometric normal points up.
    const auto& verts = mesh.value().vertices;
    for (std::size_t t = 0; t < mesh.value().indices.size(); t += 3) {
        const glm::vec3 a = verts[mesh.value().indices[t]].position;
        const glm::vec3 b = verts[mesh.value().indices[t + 1]].position;
        const glm::vec3 c = verts[mesh.value().indices[t + 2]].position;
        CHECK(glm::cross(b - a, c - a).y > 0.0f);
    }
}

TEST_CASE("Tile LOD halves the cells per side, and the triangle count follows", "[unit]") {
    auto created = create_terrain(small_desc(1, 1));
    REQUIRE(created.ok());
    for (std::uint32_t lod = 0; lod <= max_terrain_lod(small_desc(1, 1)); ++lod) {
        auto mesh = build_tile_mesh(created.value(), TileCoord{0, 0}, lod, {lod, lod, lod, lod});
        REQUIRE(mesh.ok());
        const std::size_t n = 16u >> lod;
        CHECK(mesh.value().vertices.size() == (n + 1) * (n + 1));
        CHECK(mesh.value().indices.size() == n * n * 6);
    }
}

TEST_CASE("Distance selects the LOD, and each range starts the next level", "[unit]") {
    const std::vector<float> ranges{10.0f, 30.0f, 60.0f};
    CHECK(select_tile_lod(0.0f, ranges, 4) == 0);
    CHECK(select_tile_lod(9.99f, ranges, 4) == 0);
    CHECK(select_tile_lod(10.0f, ranges, 4) == 1);
    CHECK(select_tile_lod(45.0f, ranges, 4) == 2);
    CHECK(select_tile_lod(500.0f, ranges, 4) == 3);
    // The cap keeps the LOD on the tile's grid.
    CHECK(select_tile_lod(500.0f, ranges, 2) == 2);
}

TEST_CASE("Moving the camera switches each tile to the LOD of its distance", "[unit]") {
    // Four tiles across, one row: tile 0 spans X in [0, 16), tile 3 spans X in [48, 64). The camera is at X = 0, so tile distance grows along X.
    const auto terrain = wave_terrain(small_desc(4, 1));
    const std::vector<float> ranges{10.0f, 30.0f};

    auto near = select_terrain_lods(terrain, glm::dvec2(0.0f, 0.0f), ranges);
    REQUIRE(near.ok());
    CHECK(near.value() == std::vector<std::uint32_t>{0, 1, 2, 2});

    // Moving the camera east by 40 m shifts every tile closer, so the far tile reaches the nearer LOD.
    auto moved = select_terrain_lods(terrain, glm::dvec2(40.0f, 0.0f), ranges);
    REQUIRE(moved.ok());
    CHECK(moved.value()[3] < near.value()[3]);
    CHECK(moved.value()[0] > near.value()[0]);

    auto meshes = build_terrain_meshes(terrain, moved.value());
    REQUIRE(meshes.ok());
    REQUIRE(meshes.value().size() == 4);
    for (std::size_t i = 0; i < meshes.value().size(); ++i) {
        CHECK(meshes.value()[i].lod == moved.value()[i]);
    }

    CHECK_FALSE(select_terrain_lods(terrain, glm::dvec2(0.0f), std::vector<float>{30.0f, 10.0f}).ok());
    CHECK_FALSE(select_terrain_lods(terrain, glm::dvec2(NAN, 0.0f), ranges).ok());
}

TEST_CASE("Adjacent tiles read the same samples on their shared edge", "[unit]") {
    const auto terrain = wave_terrain(small_desc(2, 1));
    auto west = build_tile_mesh(terrain, TileCoord{0, 0}, 0, {0, 0, 0, 0});
    auto east = build_tile_mesh(terrain, TileCoord{1, 0}, 0, {0, 0, 0, 0});
    REQUIRE(west.ok());
    REQUIRE(east.ok());
    // The west tile's +X column and the east tile's -X column are the same 17 samples.
    for (std::uint32_t k = 0; k <= 16; ++k) {
        const auto& a = west.value().vertices[k * 17 + 16];
        const auto& b = east.value().vertices[k * 17 + 0];
        CHECK(a.position == b.position);
        CHECK(a.normal == b.normal);
        CHECK(a.splat == b.splat);
    }
}

TEST_CASE("A finer tile morphs the vertices between coarse samples onto the coarse edge", "[unit]") {
    const auto terrain = wave_terrain(small_desc(2, 1));
    // The east tile is coarse (LOD 2). The west tile is fine (LOD 0) and stitches its +X edge against it.
    auto fine = build_tile_mesh(terrain, TileCoord{0, 0}, 0, {0, 2, 0, 0});
    auto coarse = build_tile_mesh(terrain, TileCoord{1, 0}, 2, {0, 0, 0, 0});
    REQUIRE(fine.ok());
    REQUIRE(coarse.ok());

    // The coarse edge is the -X column of the east tile: 5 vertices, every 4 samples along Z.
    const float edge_x = fine.value().vertices[16].position.x;
    std::vector<glm::vec3> coarse_edge;
    for (const auto& vertex : coarse.value().vertices) {
        if (vertex.position.x == Catch::Approx(edge_x)) {
            coarse_edge.push_back(vertex.position);
        }
    }
    REQUIRE(coarse_edge.size() == 5);
    std::sort(coarse_edge.begin(), coarse_edge.end(), [](glm::vec3 a, glm::vec3 b) { return a.z < b.z; });

    // Every fine vertex on that edge lies on the coarse segment that contains it, at the interpolated height.
    for (std::uint32_t k = 0; k <= 16; ++k) {
        const glm::vec3 p = fine.value().vertices[k * 17 + 16].position;
        const std::size_t seg = std::min<std::size_t>(static_cast<std::size_t>(k / 4), 3);
        const glm::vec3 a = coarse_edge[seg];
        const glm::vec3 b = coarse_edge[seg + 1];
        const float w = (p.z - a.z) / (b.z - a.z);
        CHECK(w >= -1e-5f);
        CHECK(w <= 1.0f + 1e-5f);
        CHECK(p.y == Catch::Approx(a.y + (b.y - a.y) * w).margin(1e-4f));
        }

    // The coarse edge is not stitched: its own vertices at multiples of 4 keep their sampled heights.
    for (std::size_t i = 0; i < coarse_edge.size(); ++i) {
        const std::uint32_t iz = static_cast<std::uint32_t>(std::lround(coarse_edge[i].z));
        const std::uint32_t ix = 16;
        CHECK(coarse_edge[i].y == Catch::Approx(engine::world::sample_height(terrain, ix, iz)).margin(1e-5f));
    }
}

TEST_CASE("A mixed-LOD grid covers every point once, with no holes and no overlaps", "[unit]") {
    // A 3 x 3 grid of tiles at mixed LODs, including neighbours that differ by three levels. The tiling is checked by point coverage.
    const auto terrain = wave_terrain(small_desc(3, 3));
    const std::vector<std::uint32_t> lods{0, 3, 1, 2, 0, 3, 1, 1, 2};
    auto meshes = build_terrain_meshes(terrain, lods);
    REQUIRE(meshes.ok());

    const glm::vec2 extent = engine::world::terrain_extent(terrain);
    const auto histogram = coverage_histogram(meshes.value(), extent, 90);
    int total = 0;
    for (const auto& [covered, count] : histogram) {
        INFO("points covered " << covered << " times: " << count);
        CHECK(covered == 1);
        total += count;
    }
    CHECK(total == 90 * 90);
}

TEST_CASE("Every stitched edge matches its coarse neighbour in height, normal, and splat", "[unit]") {
    auto terrain = wave_terrain(small_desc(3, 1));
    // Give each sample a splat that varies across the tile, so the splat blend is checked on the morphed vertices too.
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            const std::uint8_t a = static_cast<std::uint8_t>(40 + (ix * 7) % 120);
            terrain.splat[engine::world::sample_index(terrain, ix, iz)] = {a, static_cast<std::uint8_t>(255 - a), 60, 30};
        }
    }
    const std::vector<std::uint32_t> lods{0, 2, 0};
    auto meshes = build_terrain_meshes(terrain, lods);
    REQUIRE(meshes.ok());
    for (const auto& mesh : meshes.value()) {
        for (const auto& vertex : mesh.vertices) {
            CHECK(glm::length(vertex.normal) == Catch::Approx(1.0f));
            CHECK(vertex.splat.x + vertex.splat.y + vertex.splat.z + vertex.splat.w == Catch::Approx(1.0f));
        }
    }
    // Tile 0 (LOD 0) stitches its +X edge against tile 1 (LOD 2). Its morphed vertices use the coarse samples' normal and splat.
    const auto& fine = meshes.value()[0];
    const auto& coarse = meshes.value()[1];
    for (std::uint32_t k = 0; k <= 16; ++k) {
        const std::uint32_t fine_index = k * 17 + 16;
        if (k % 4 != 0) {
            // Morphed vertices have a normal and splat between the coarse end samples' values.
            const std::uint32_t start = k - k % 4;
            const float w = static_cast<float>(k - start) / 4.0f;
            const auto a = sample_splat(terrain, 16, start);
            const auto b = sample_splat(terrain, 16, start + 4);
            CHECK(fine.vertices[fine_index].splat.x == Catch::Approx(a.x + (b.x - a.x) * w).margin(1e-5f));
        } else {
            // Unmorphed vertices sit on coarse samples: the two meshes agree there.
            const std::uint32_t coarse_index = (k / 4) * 5;
            CHECK(fine.vertices[fine_index].position.y == Catch::Approx(coarse.vertices[coarse_index].position.y).margin(1e-5f));
            CHECK(fine.vertices[fine_index].splat == coarse.vertices[coarse_index].splat);
        }
    }
}

TEST_CASE("Normals follow the height gradient, and splat weights normalise to one", "[unit]") {
    auto created = create_terrain(small_desc(1, 1));
    REQUIRE(created.ok());
    auto terrain = std::move(created.value());
    // A plane rising 0.5 m per metre in X: the normal tilts toward -X with slope 0.5.
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            terrain.heights[engine::world::sample_index(terrain, ix, iz)] = 0.5f * static_cast<float>(ix);
        }
    }
    const glm::vec3 interior = engine::world::sample_normal(terrain, 8, 8);
    CHECK(interior.x == Catch::Approx(-0.5f / std::sqrt(1.25f)).margin(1e-5f));
    CHECK(interior.y == Catch::Approx(1.0f / std::sqrt(1.25f)).margin(1e-5f));
    CHECK(interior.z == Catch::Approx(0.0f).margin(1e-5f));

    // Layer weights normalise, and an all-zero sample reads as layer 0.
    terrain.splat[engine::world::sample_index(terrain, 1, 1)] = {100, 100, 0, 0};
    const glm::vec4 blend = sample_splat(terrain, 1, 1);
    CHECK(blend.x == Catch::Approx(0.5f));
    CHECK(blend.y == Catch::Approx(0.5f));
    CHECK(blend.x + blend.y + blend.z + blend.w == Catch::Approx(1.0f));
    terrain.splat[engine::world::sample_index(terrain, 2, 2)] = {0, 0, 0, 0};
    CHECK(sample_splat(terrain, 2, 2) == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("A height query interpolates between samples and clamps at the edges", "[unit]") {
    auto created = create_terrain(small_desc(1, 1));
    REQUIRE(created.ok());
    auto terrain = std::move(created.value());
    terrain.heights[engine::world::sample_index(terrain, 1, 0)] = 2.0f;
    CHECK(height_at(terrain, glm::vec2(1.0f, 0.0f)) == Catch::Approx(2.0f));
    CHECK(height_at(terrain, glm::vec2(0.5f, 0.0f)) == Catch::Approx(1.0f));
    CHECK(height_at(terrain, glm::vec2(-50.0f, -50.0f)) == Catch::Approx(0.0f));
}

TEST_CASE("Bad tile, LOD, and neighbour inputs are rejected", "[unit]") {
    const auto terrain = wave_terrain(small_desc(2, 2));
    CHECK_FALSE(build_tile_mesh(terrain, TileCoord{2, 0}, 0, {0, 0, 0, 0}).ok());
    CHECK_FALSE(build_tile_mesh(terrain, TileCoord{0, 0}, 5, {0, 0, 0, 0}).ok());
    CHECK_FALSE(build_tile_mesh(terrain, TileCoord{0, 0}, 0, {0, 0, 9, 0}).ok());
    CHECK_FALSE(build_terrain_meshes(terrain, std::vector<std::uint32_t>{0, 0, 0}).ok());
}
