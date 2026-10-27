// Terrain editing session (US-071) in the editor model: a stroke is one undo command, a refresh invalidates exactly the tiles the stroke
// touched, a mesh asked for after an undo (with no refresh) is the reverted one, the tile bodies follow the edit, and save and open keep
// the grid. No window, graphics device, or ImGui is used.

#include "engine/core/path.hpp"
#include "engine/ecs/world.hpp"
#include "engine/editor/terrain_editor.hpp"
#include "engine/physics/collision_cells.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using engine::core::Path;
using engine::ecs::World;
using engine::editor::TerrainEditor;
using engine::physics::PhysicsWorld;
using engine::world::Brush;
using engine::world::BrushKind;
using engine::world::TerrainDesc;
using engine::world::TileCoord;
namespace fs = std::filesystem;

namespace {

// Two by one tiles of 8 cells each, so the shared edge is at x = 8 and a stamp at x = 4 touches tile 0 alone.
TerrainDesc small_desc() {
    TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 1;
    desc.tile_cells = 8;
    desc.cell_size = 1.0f;
    return desc;
}

TerrainEditor small_editor() {
    auto created = TerrainEditor::create(small_desc());
    REQUIRE(created.ok());
    return std::move(created.value());
}

// One full-strength raise stamp, hard-edged, so the touched samples are exactly those within the radius of the centre.
void raise_at(TerrainEditor& editor, glm::vec2 centre) {
    Brush raise;
    raise.kind = BrushKind::Raise;
    raise.radius = 1.0f;
    raise.strength = 1.0f;
    raise.falloff = 0.0f;
    editor.brush() = raise;
    REQUIRE(editor.begin_stroke().ok());
    REQUIRE(editor.stamp(centre).ok());
    REQUIRE(editor.end_stroke().ok());
}

}  // namespace

TEST_CASE("A terrain editor refuses an invalid description and leaves no session", "[unit][editor]") {
    TerrainDesc bad = small_desc();
    bad.tiles_x = 0;
    CHECK_FALSE(TerrainEditor::create(bad).ok());
}

TEST_CASE("A stroke is one undo command, and undo and redo move the terrain by it", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    const float before = engine::world::sample_height(editor.terrain(), 4, 4);

    raise_at(editor, glm::vec2(4.0f, 4.0f));
    CHECK(editor.document().dirty());
    const float after = engine::world::sample_height(editor.terrain(), 4, 4);
    CHECK(after > before);

    CHECK(editor.undo());
    CHECK(engine::world::sample_height(editor.terrain(), 4, 4) == before);
    CHECK_FALSE(editor.undo());

    CHECK(editor.redo());
    CHECK(engine::world::sample_height(editor.terrain(), 4, 4) == after);
    CHECK_FALSE(editor.redo());
}

TEST_CASE("A refresh invalidates only the tiles the stroke touched, and a rebuilt mesh matches the terrain", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    REQUIRE(editor.tile_mesh(TileCoord{0, 0}).ok());
    REQUIRE(editor.tile_mesh(TileCoord{1, 0}).ok());

    raise_at(editor, glm::vec2(4.0f, 4.0f));
    const engine::editor::TerrainRefresh report = editor.refresh(nullptr);
    REQUIRE(report.invalidated.size() == 1);
    CHECK(report.invalidated[0] == TileCoord{0, 0});
    CHECK(report.colliders_refreshed.empty());

    // The rebuilt mesh of tile 0 carries the raised sample at its local (4, 4), and tile 1 is still the untouched flat mesh.
    const auto mesh0 = editor.tile_mesh(TileCoord{0, 0});
    REQUIRE(mesh0.ok());
    const std::uint32_t side = 9;
    CHECK(mesh0.value()->vertices[4 * side + 4].position.y == Catch::Approx(engine::world::sample_height(editor.terrain(), 4, 4)));
    const auto mesh1 = editor.tile_mesh(TileCoord{1, 0});
    REQUIRE(mesh1.ok());
    CHECK(mesh1.value()->vertices[4 * side + 4].position.y == Catch::Approx(0.0f));

    // A refresh with nothing dirty invalidates nothing.
    CHECK(editor.refresh(nullptr).invalidated.empty());
}

TEST_CASE("A mesh asked for after an undo, with no refresh between, is the reverted mesh", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    raise_at(editor, glm::vec2(4.0f, 4.0f));
    const auto raised = editor.tile_mesh(TileCoord{0, 0});
    REQUIRE(raised.ok());
    const float raised_height = raised.value()->vertices[4 * 9 + 4].position.y;
    REQUIRE(raised_height > 0.0f);

    REQUIRE(editor.undo());
    const auto reverted = editor.tile_mesh(TileCoord{0, 0});
    REQUIRE(reverted.ok());
    CHECK(reverted.value()->vertices[4 * 9 + 4].position.y == Catch::Approx(0.0f));
}

TEST_CASE("A tile outside the terrain has no mesh", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    CHECK_FALSE(editor.tile_mesh(TileCoord{5, 0}).ok());
}

TEST_CASE("Tile bodies follow an edit: a refresh rebuilds the touched body, and a raycast sees the new height", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    World ecs;
    auto created = PhysicsWorld::create(engine::physics::PhysicsWorldDesc{});
    REQUIRE(created.ok());
    std::unique_ptr<PhysicsWorld> physics = std::move(created).value();

    REQUIRE(editor.ensure_colliders(*physics, ecs).ok());
    CHECK(editor.has_collider(TileCoord{0, 0}));
    CHECK(editor.has_collider(TileCoord{1, 0}));
    CHECK(physics->body_count() == 2);

    // Four stamps raise the centre sample by one metre. Without a refresh the body still has the old shape.
    for (int i = 0; i < 4; ++i) {
        raise_at(editor, glm::vec2(4.0f, 4.0f));
    }
    const auto stale = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(stale.has_value());
    CHECK(stale->position.y == Catch::Approx(0.0).margin(1e-3));

    const engine::editor::TerrainRefresh report = editor.refresh(physics.get());
    REQUIRE(report.colliders_refreshed.size() == 1);
    CHECK(report.colliders_refreshed[0] == TileCoord{0, 0});
    CHECK(report.collider_failures.empty());
    CHECK(physics->body_count() == 2);

    const auto after = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(after.has_value());
    CHECK(after->position.y == Catch::Approx(engine::world::sample_height(editor.terrain(), 4, 4)).margin(1e-2));
    CHECK(after->position.y == Catch::Approx(1.0).margin(1e-2));
}

TEST_CASE("An undo without a refresh is applied to the body on the next refresh that has the physics world", "[unit][editor]") {
    TerrainEditor editor = small_editor();
    World ecs;
    auto created = PhysicsWorld::create(engine::physics::PhysicsWorldDesc{});
    REQUIRE(created.ok());
    std::unique_ptr<PhysicsWorld> physics = std::move(created).value();
    REQUIRE(editor.ensure_colliders(*physics, ecs).ok());

    raise_at(editor, glm::vec2(4.0f, 4.0f));
    // A refresh with no physics world invalidates the mesh but leaves the body alone.
    CHECK(editor.refresh(nullptr).colliders_refreshed.empty());
    CHECK(editor.undo());
    const engine::editor::TerrainRefresh report = editor.refresh(physics.get());
    REQUIRE(report.colliders_refreshed.size() == 1);

    const auto back = physics->raycast(glm::dvec3(4.0, 10.0, 4.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(back.has_value());
    CHECK(back->position.y == Catch::Approx(0.0).margin(1e-2));
}

TEST_CASE("Save and open keep the grid, and a save clears the dirty state", "[unit][editor]") {
    const fs::path root = fs::temp_directory_path() / "engine_terrain_editor_tests";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);

    TerrainEditor editor = small_editor();
    raise_at(editor, glm::vec2(4.0f, 4.0f));
    raise_at(editor, glm::vec2(12.0f, 4.0f));
    CHECK(editor.document().dirty());

    const auto path = Path::from_utf8((root / "landscape.terrain").string());
    REQUIRE(path.ok());
    REQUIRE(editor.save(path.value()).ok());
    CHECK_FALSE(editor.document().dirty());

    auto reopened = TerrainEditor::open(path.value());
    REQUIRE(reopened.ok());
    CHECK(reopened.value().terrain().heights == editor.terrain().heights);
    CHECK(reopened.value().terrain().splat == editor.terrain().splat);
    CHECK_FALSE(reopened.value().document().dirty());
    CHECK_FALSE(reopened.value().document().can_undo());

    // The reopened mesh matches the saved one, so the cooked result of either session is the same.
    const auto a = editor.tile_mesh(TileCoord{1, 0});
    const auto b = reopened.value().tile_mesh(TileCoord{1, 0});
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value()->vertices.size() == b.value()->vertices.size());
    CHECK(a.value()->indices == b.value()->indices);

    const auto missing = Path::from_utf8((root / "missing.terrain").string());
    REQUIRE(missing.ok());
    CHECK_FALSE(TerrainEditor::open(missing.value()).ok());

    fs::remove_all(root, ignored);
}
