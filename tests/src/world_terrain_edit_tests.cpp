// Terrain sculpting and painting (US-071) on the CPU: brush weights and validation, each brush kind, splat sums, tile-edge reconciliation of
// heights and normals, one undo command per stroke, cancel, dirty tracking, the file round trip, and the physics collider that follows an
// edit. These need no renderer and run on every operating system.
#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/ecs/world.hpp"
#include "engine/physics/collision_cells.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using engine::core::Guid;
using engine::core::Path;
using engine::ecs::World;
using engine::physics::PhysicsWorld;
using engine::world::Brush;
using engine::world::BrushKind;
using engine::world::Terrain;
using engine::world::TerrainDesc;
using engine::world::TerrainDocument;
using engine::world::TileCoord;
namespace fs = std::filesystem;

namespace {

// Two by one tiles of 8 cells each: a 16 m by 8 m terrain, with one shared edge at x = 8.
TerrainDesc small_desc() {
    TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 1;
    desc.tile_cells = 8;
    desc.cell_size = 1.0f;
    return desc;
}

TerrainDocument small_document() {
    auto doc = TerrainDocument::create(small_desc());
    REQUIRE(doc.ok());
    return std::move(doc.value());
}

Brush brush(BrushKind kind, float radius, float strength, float falloff) {
    Brush b;
    b.kind = kind;
    b.radius = radius;
    b.strength = strength;
    b.falloff = falloff;
    return b;
}

std::vector<float> heights_of(const Terrain& terrain) {
    return terrain.heights;
}

// Hard-edged raise of one unit of strength: each stamp adds the brush's height step at its centre.
void stamp_at(TerrainDocument& doc, const Brush& b, glm::vec2 centre) {
    REQUIRE(doc.begin_stroke(b).ok());
    REQUIRE(doc.stamp(centre).ok());
    REQUIRE(doc.end_stroke().ok());
}

}  // namespace

TEST_CASE("Brush weight is one inside the core, eases across the falloff, and is zero at the radius", "[unit]") {
    CHECK(engine::world::brush_weight(0.0f, 0.5f) == 1.0f);
    CHECK(engine::world::brush_weight(0.4f, 0.5f) == 1.0f);
    CHECK(engine::world::brush_weight(1.0f, 0.5f) == 0.0f);
    CHECK(engine::world::brush_weight(1.5f, 0.0f) == 0.0f);
    // A hard edge: full weight right up to the radius.
    CHECK(engine::world::brush_weight(0.99f, 0.0f) == 1.0f);

    const float near = engine::world::brush_weight(0.6f, 0.5f);
    const float far = engine::world::brush_weight(0.9f, 0.5f);
    CHECK(near > far);
    CHECK(near > 0.0f);
    CHECK(near < 1.0f);
}

TEST_CASE("Brush descriptions outside their ranges are rejected", "[unit]") {
    CHECK(engine::world::validate_brush(brush(BrushKind::Raise, 2.0f, 0.5f, 0.5f)).ok());
    CHECK_FALSE(engine::world::validate_brush(brush(BrushKind::Raise, 0.0f, 0.5f, 0.5f)).ok());
    CHECK_FALSE(engine::world::validate_brush(brush(BrushKind::Raise, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.5f)).ok());
    CHECK_FALSE(engine::world::validate_brush(brush(BrushKind::Raise, 2.0f, 0.0f, 0.5f)).ok());
    CHECK_FALSE(engine::world::validate_brush(brush(BrushKind::Raise, 2.0f, 1.5f, 0.5f)).ok());
    CHECK_FALSE(engine::world::validate_brush(brush(BrushKind::Raise, 2.0f, 0.5f, -0.1f)).ok());

    Brush paint = brush(BrushKind::Paint, 2.0f, 0.5f, 0.5f);
    paint.layer = engine::world::kTerrainLayers;
    CHECK_FALSE(engine::world::validate_brush(paint).ok());
}

TEST_CASE("Raise adds the step at the centre and lower takes it back exactly", "[unit]") {
    TerrainDocument doc = small_document();
    const Brush raise = brush(BrushKind::Raise, 2.0f, 0.5f, 0.0f);
    stamp_at(doc, raise, glm::vec2(4.0f, 4.0f));

    const Terrain& t = doc.terrain();
    const float step = engine::world::kBrushHeightStep * 0.5f;
    CHECK(engine::world::sample_height(t, 4, 4) == Catch::Approx(step));
    // Inside the radius at a hard edge, one sample away still rises; at the radius it does not.
    CHECK(engine::world::sample_height(t, 5, 4) == Catch::Approx(step));
    CHECK(engine::world::sample_height(t, 6, 4) == 0.0f);

    stamp_at(doc, brush(BrushKind::Lower, 2.0f, 0.5f, 0.0f), glm::vec2(4.0f, 4.0f));
    CHECK(engine::world::sample_height(t, 4, 4) == 0.0f);
    CHECK(engine::world::sample_height(t, 5, 4) == 0.0f);
}

TEST_CASE("Smooth pulls a spike toward its neighbours and flatten pulls toward its target", "[unit]") {
    TerrainDocument doc = small_document();
    // Build a one-sample spike by raising the centre through a stroke, then smooth it.
    Brush spike = brush(BrushKind::Flatten, 0.5f, 1.0f, 0.0f);
    spike.target_height = 1.0f;
    stamp_at(doc, spike, glm::vec2(4.0f, 4.0f));
    CHECK(engine::world::sample_height(doc.terrain(), 4, 4) == Catch::Approx(1.0f));

    stamp_at(doc, brush(BrushKind::Smooth, 1.2f, 1.0f, 0.0f), glm::vec2(4.0f, 4.0f));
    // The centre's 3x3 average before the stamp was 1/9, and a full-strength smooth takes it all the way.
    CHECK(engine::world::sample_height(doc.terrain(), 4, 4) == Catch::Approx(1.0f / 9.0f).margin(1e-5f));

    Brush flat = brush(BrushKind::Flatten, 2.0f, 1.0f, 0.0f);
    flat.target_height = 3.0f;
    stamp_at(doc, flat, glm::vec2(10.0f, 4.0f));
    CHECK(engine::world::sample_height(doc.terrain(), 10, 4) == Catch::Approx(3.0f));
    CHECK(engine::world::sample_height(doc.terrain(), 9, 4) == Catch::Approx(3.0f));
    CHECK(engine::world::sample_height(doc.terrain(), 12, 4) == 0.0f);
}

TEST_CASE("Paint moves the layer weight toward the brush and keeps every sample's weights summing to 255", "[unit]") {
    TerrainDocument doc = small_document();
    Brush paint = brush(BrushKind::Paint, 2.0f, 1.0f, 0.0f);
    paint.layer = 2;
    stamp_at(doc, paint, glm::vec2(4.0f, 4.0f));

    const Terrain& t = doc.terrain();
    const glm::vec4 centre = engine::world::sample_splat(t, 4, 4);
    CHECK(centre.z == Catch::Approx(1.0f));
    CHECK(centre.x == Catch::Approx(0.0f));

    // A half-strength paint leaves layer 0 and layer 2 sharing the sample, with an exact sum of 255 everywhere.
    TerrainDocument half = small_document();
    paint.strength = 0.5f;
    stamp_at(half, paint, glm::vec2(4.0f, 4.0f));
    for (std::uint32_t iz = 0; iz < half.terrain().samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < half.terrain().samples_x; ++ix) {
            int sum = 0;
            for (std::uint8_t w : half.terrain().splat[engine::world::sample_index(half.terrain(), ix, iz)]) {
                sum += w;
            }
            CHECK(sum == 255);
        }
    }
    CHECK(half.terrain().splat[engine::world::sample_index(half.terrain(), 4, 4)][2] == 128);
}

TEST_CASE("A stroke across a tile edge reconciles heights and normals on both tiles and dirties both", "[unit]") {
    TerrainDocument doc = small_document();
    const Brush raise = brush(BrushKind::Raise, 3.0f, 1.0f, 0.5f);
    REQUIRE(doc.begin_stroke(raise).ok());
    REQUIRE(doc.stamp(glm::vec2(8.0f, 4.0f)).ok());  // Column 8 is the shared edge.
    REQUIRE(doc.end_stroke().ok());

    const std::vector<TileCoord> dirty = doc.take_dirty_tiles();
    REQUIRE(dirty.size() == 2);
    CHECK(dirty[0].x == 0);
    CHECK(dirty[1].x == 1);
    CHECK(doc.tile_revision(TileCoord{0, 0}) == 1);
    CHECK(doc.tile_revision(TileCoord{1, 0}) == 1);
    CHECK(doc.take_dirty_tiles().empty());

    // Each tile's +X edge vertices (tile 0) and -X edge vertices (tile 1) describe the same samples: heights and normals agree.
    const auto left = engine::world::build_tile_mesh(doc.terrain(), TileCoord{0, 0}, 0, {0, 0, 0, 0});
    const auto right = engine::world::build_tile_mesh(doc.terrain(), TileCoord{1, 0}, 0, {0, 0, 0, 0});
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    const std::uint32_t side = 9;
    for (std::uint32_t iz = 0; iz < side; ++iz) {
        const auto& a = left.value().vertices[iz * side + 8];
        const auto& b = right.value().vertices[iz * side];
        CHECK(a.position.y == Catch::Approx(b.position.y).margin(1e-6f));
        CHECK(a.normal.x == Catch::Approx(b.normal.x).margin(1e-6f));
        CHECK(a.normal.y == Catch::Approx(b.normal.y).margin(1e-6f));
        CHECK(a.normal.z == Catch::Approx(b.normal.z).margin(1e-6f));
    }
    CHECK(left.value().vertices[4 * side + 8].position.y > 0.0f);
}

TEST_CASE("A stroke of several stamps is one undo command, and redo restores it", "[unit]") {
    TerrainDocument doc = small_document();
    const Terrain before = doc.terrain();

    REQUIRE(doc.begin_stroke(brush(BrushKind::Raise, 2.0f, 1.0f, 0.5f)).ok());
    REQUIRE(doc.stamp(glm::vec2(2.0f, 2.0f)).ok());
    REQUIRE(doc.stamp(glm::vec2(2.5f, 2.0f)).ok());
    REQUIRE(doc.stamp(glm::vec2(12.0f, 4.0f)).ok());
    REQUIRE(doc.stamp(glm::vec2(2.0f, 2.0f)).ok());
    REQUIRE(doc.stamp(glm::vec2(8.0f, 4.0f)).ok());
    CHECK(doc.stroke_open());
    REQUIRE(doc.end_stroke().ok());
    const Terrain after = doc.terrain();
    CHECK(heights_of(after) != heights_of(before));

    CHECK(doc.can_undo());
    CHECK(doc.undo());
    CHECK(heights_of(doc.terrain()) == heights_of(before));
    CHECK(doc.terrain().splat == before.splat);
    CHECK_FALSE(doc.can_undo());
    CHECK_FALSE(doc.undo());

    CHECK(doc.can_redo());
    CHECK(doc.redo());
    CHECK(heights_of(doc.terrain()) == heights_of(after));
    CHECK_FALSE(doc.can_redo());
}

TEST_CASE("A stroke that changes nothing adds no command, and cancel restores the samples it touched", "[unit]") {
    TerrainDocument doc = small_document();

    REQUIRE(doc.begin_stroke(brush(BrushKind::Raise, 1.0f, 1.0f, 0.5f)).ok());
    REQUIRE(doc.stamp(glm::vec2(-50.0f, -50.0f)).ok());
    const auto unchanged = doc.end_stroke();
    REQUIRE(unchanged.ok());
    CHECK_FALSE(unchanged.value());
    CHECK_FALSE(doc.can_undo());
    CHECK_FALSE(doc.dirty());

    const Terrain before = doc.terrain();
    REQUIRE(doc.begin_stroke(brush(BrushKind::Lower, 2.0f, 1.0f, 0.5f)).ok());
    REQUIRE(doc.stamp(glm::vec2(4.0f, 4.0f)).ok());
    REQUIRE(doc.stamp(glm::vec2(12.0f, 4.0f)).ok());
    REQUIRE(doc.cancel_stroke().ok());
    CHECK_FALSE(doc.stroke_open());
    CHECK(heights_of(doc.terrain()) == heights_of(before));
    CHECK_FALSE(doc.can_undo());
}

TEST_CASE("Stroke calls are checked: a stamp needs an open stroke, and a second stroke cannot open over one", "[unit]") {
    TerrainDocument doc = small_document();
    CHECK_FALSE(doc.stamp(glm::vec2(1.0f, 1.0f)).ok());
    CHECK_FALSE(doc.end_stroke().ok());
    REQUIRE(doc.begin_stroke(brush(BrushKind::Raise, 1.0f, 1.0f, 0.5f)).ok());
    CHECK_FALSE(doc.begin_stroke(brush(BrushKind::Raise, 1.0f, 1.0f, 0.5f)).ok());
    CHECK(doc.cancel_stroke().ok());
}

TEST_CASE("A new edit after an undo discards the redo history, and the dirty flag follows the saved point", "[unit]") {
    TerrainDocument doc = small_document();
    CHECK_FALSE(doc.dirty());

    stamp_at(doc, brush(BrushKind::Raise, 2.0f, 1.0f, 0.5f), glm::vec2(4.0f, 4.0f));
    CHECK(doc.dirty());
    doc.mark_saved();
    CHECK_FALSE(doc.dirty());

    CHECK(doc.undo());
    CHECK(doc.dirty());
    CHECK(doc.redo());
    CHECK_FALSE(doc.dirty());

    CHECK(doc.undo());
    stamp_at(doc, brush(BrushKind::Lower, 2.0f, 1.0f, 0.5f), glm::vec2(4.0f, 4.0f));
    CHECK_FALSE(doc.can_redo());
    // The saved state is gone from the history, so the document is dirty even though it is back at the saved heights.
    CHECK(doc.dirty());
}

TEST_CASE("Tile revisions count only the tiles an edit changed, and the dirty list is row-major and unique", "[unit]") {
    TerrainDocument doc = small_document();
    CHECK(doc.tile_revision(TileCoord{1, 0}) == 0);
    CHECK(doc.tile_revision(TileCoord{9, 9}) == 0);

    stamp_at(doc, brush(BrushKind::Raise, 1.5f, 1.0f, 0.5f), glm::vec2(2.0f, 4.0f));
    CHECK(doc.tile_revision(TileCoord{0, 0}) == 1);
    CHECK(doc.tile_revision(TileCoord{1, 0}) == 0);
    const auto dirty = doc.take_dirty_tiles();
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0].x == 0);
    CHECK(dirty[0].z == 0);

    CHECK(doc.undo());
    CHECK(doc.tile_revision(TileCoord{0, 0}) == 2);
}

TEST_CASE("A terrain file reopens with exactly the same grid, and a damaged file is refused", "[unit]") {
    TerrainDocument doc = small_document();
    stamp_at(doc, brush(BrushKind::Raise, 3.0f, 0.7f, 0.3f), glm::vec2(8.0f, 4.0f));
    Brush paint = brush(BrushKind::Paint, 3.0f, 0.4f, 0.5f);
    paint.layer = 3;
    stamp_at(doc, paint, glm::vec2(7.0f, 3.0f));

    const std::string bytes = engine::world::serialize_terrain(doc.terrain());
    auto reopened = engine::world::parse_terrain(bytes);
    REQUIRE(reopened.ok());
    CHECK(heights_of(reopened.value()) == heights_of(doc.terrain()));
    CHECK(reopened.value().splat == doc.terrain().splat);
    CHECK(reopened.value().desc.tile_cells == doc.terrain().desc.tile_cells);
    CHECK(engine::world::serialize_terrain(reopened.value()) == bytes);

    CHECK_FALSE(engine::world::parse_terrain(bytes.substr(0, bytes.size() - 1)).ok());
    std::string bad_magic = bytes;
    bad_magic[0] = 'X';
    CHECK_FALSE(engine::world::parse_terrain(bad_magic).ok());

    std::string nan_height = bytes;
    const std::uint32_t nan_bits = 0x7fc00000u;
    for (int i = 0; i < 4; ++i) {
        nan_height[32 + static_cast<std::size_t>(i)] = static_cast<char>((nan_bits >> (8 * i)) & 0xffu);
    }
    CHECK_FALSE(engine::world::parse_terrain(nan_height).ok());
}

TEST_CASE("Saving a terrain writes it atomically, and reopening it reads the same grid back", "[integration]") {
    const fs::path root = fs::temp_directory_path() / ("engine_terrain_edit_" + Guid::generate().to_string());
    fs::create_directories(root);
    const auto path = Path::from_utf8((root / "landscape.terrain").string());
    REQUIRE(path.ok());

    TerrainDocument doc = small_document();
    stamp_at(doc, brush(BrushKind::Smooth, 2.0f, 0.5f, 0.5f), glm::vec2(8.0f, 4.0f));
    REQUIRE(engine::world::save_terrain(path.value(), doc.terrain()).ok());
    auto loaded = engine::world::load_terrain(path.value());
    REQUIRE(loaded.ok());
    CHECK(heights_of(loaded.value()) == heights_of(doc.terrain()));
    CHECK(loaded.value().splat == doc.terrain().splat);

    const auto missing = Path::from_utf8((root / "missing.terrain").string());
    REQUIRE(missing.ok());
    CHECK_FALSE(engine::world::load_terrain(missing.value()).ok());

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

TEST_CASE("An edit one sample inside a tile dirties the neighbour whose edge normal reads it", "[unit]") {
    TerrainDocument doc = small_document();
    const auto before = engine::world::build_tile_mesh(doc.terrain(), TileCoord{0, 0}, 0, {0, 0, 0, 0});
    REQUIRE(before.ok());

    // Sample column 9 belongs to tile 1 alone. The normal of tile 0's edge sample (column 8) reads column 9, so tile 0 must be dirty.
    stamp_at(doc, brush(BrushKind::Raise, 0.6f, 1.0f, 0.0f), glm::vec2(9.0f, 4.0f));
    const std::vector<TileCoord> dirty = doc.take_dirty_tiles();
    REQUIRE(dirty.size() == 2);
    CHECK(dirty[0].x == 0);
    CHECK(dirty[1].x == 1);

    const auto after = engine::world::build_tile_mesh(doc.terrain(), TileCoord{0, 0}, 0, {0, 0, 0, 0});
    REQUIRE(after.ok());
    const std::uint32_t side = 9;
    const bool changed = after.value().vertices[4 * side + 8].normal != before.value().vertices[4 * side + 8].normal;
    CHECK(changed);
}

TEST_CASE("An edited tile's collider is rebuilt in place, and a raycast sees the new height", "[unit]") {
    TerrainDocument doc = small_document();
    const Terrain& base = doc.terrain();

    World ecs;
    auto created = PhysicsWorld::create(engine::physics::PhysicsWorldDesc{});
    REQUIRE(created.ok());
    std::unique_ptr<PhysicsWorld> physics = std::move(created).value();

    auto cell = engine::physics::terrain_tile_cell(base, TileCoord{0, 0}, 1, ecs.create());
    REQUIRE(cell.ok());
    const auto entity = physics->add_body(cell.value().bodies.front());
    REQUIRE(entity.ok());

    const auto before = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(before.has_value());
    CHECK(before->position.y == Catch::Approx(0.0).margin(1e-3));

    // Four full-strength stamps raise the centre sample by one metre; the body still holds the old shape until it is refreshed.
    for (int i = 0; i < 4; ++i) {
        stamp_at(doc, brush(BrushKind::Raise, 1.0f, 1.0f, 0.0f), glm::vec2(4.0f, 4.0f));
    }
    const auto stale = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(stale.has_value());
    CHECK(stale->position.y == Catch::Approx(0.0).margin(1e-3));

    const auto dirty = doc.take_dirty_tiles();
    REQUIRE(dirty.size() == 1);
    REQUIRE(engine::physics::refresh_terrain_collider(*physics, doc.terrain(), dirty[0], entity.value()).ok());
    CHECK(physics->body_count() == 1);

    const auto after = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(after.has_value());
    CHECK(after->entity == entity.value());
    // Jolt quantises heightfield samples, so the collider agrees with the terrain to about a centimetre, not exactly.
    CHECK(after->position.y == Catch::Approx(engine::world::sample_height(doc.terrain(), 4, 4)).margin(1e-2));
    CHECK(after->position.y == Catch::Approx(1.0).margin(1e-2));

    // A tile outside the terrain cannot be refreshed, and the body keeps its shape.
    CHECK_FALSE(engine::physics::refresh_terrain_collider(*physics, doc.terrain(), TileCoord{5, 5}, entity.value()).ok());
    const auto kept = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(kept.has_value());
    CHECK(kept->position.y == Catch::Approx(1.0).margin(1e-2));
}
