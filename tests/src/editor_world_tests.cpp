// The world partition editor (US-072), headless: creating and opening a world directory, moving objects between cells with their GUIDs,
// references, and dirty state kept, saving only dirty cells, and residency controls that never lose an edit. The editor's streaming
// reads the stream cache written from the in-memory world, so these cases poll tick() until the cells arrive.

#include "engine/editor/world_editor.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

using engine::core::Guid;
using engine::core::Path;
using engine::editor::WorldEditor;
using engine::world::CellCoord;
using engine::world::CellPhase;
using engine::world::WorldDesc;
using engine::world::WorldObject;
namespace fs = std::filesystem;

namespace {

// A point inside the cell at (x, z), away from its boundaries, with the default 256 m cells.
glm::vec3 at_cell(std::int32_t x, std::int32_t z) {
    return glm::vec3(static_cast<float>(x) * 256.0f + 128.0f, 0.0f, static_cast<float>(z) * 256.0f + 128.0f);
}

WorldObject object_named(const std::string& name, glm::vec3 position) {
    WorldObject object;
    object.id = Guid::generate();
    object.name = name;
    object.asset = Guid::generate();
    object.position = position;
    return object;
}

// Removes the scratch directory when the test ends, even if a check failed part way through.
struct ScratchGuard {
    fs::path directory;
    explicit ScratchGuard(fs::path dir) : directory(std::move(dir)) {}
    ~ScratchGuard() {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
    ScratchGuard(const ScratchGuard&) = delete;
    ScratchGuard& operator=(const ScratchGuard&) = delete;
};

fs::path scratch_dir() {
    return fs::temp_directory_path() / ("engine_world_editor_" + Guid::generate().to_string());
}

Path path_of(const fs::path& directory) {
    auto path = Path::from_utf8(directory.string());
    return std::move(path.value());
}

// Ticks the editor until `done` holds or the timeout passes. Each tick is one residency update and safe point.
template <typename Done>
bool tick_until(WorldEditor& editor, Done done) {
    for (int attempt = 0; attempt < 4000; ++attempt) {
        REQUIRE(editor.tick().ok());
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

WorldEditor make_world(const fs::path& directory) {
    auto created = WorldEditor::create(WorldDesc{}, path_of(directory));
    REQUIRE(created.ok());
    return std::move(created.value());
}

}  // namespace

TEST_CASE("A new world refuses an invalid grid and creates nothing", "[unit][editor]") {
    WorldDesc desc;
    desc.cell_size = 0.0f;
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);

    auto created = WorldEditor::create(desc, path_of(directory));
    CHECK_FALSE(created.ok());
    CHECK_FALSE(fs::exists(directory));
}

TEST_CASE("Moving an object between cells keeps its GUID, name, and references, and dirties both cells", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);

    const WorldObject anchor = object_named("anchor", at_cell(5, 5));
    const WorldObject lamp = object_named("lamp", at_cell(0, 0));
    REQUIRE(editor.add_object(anchor, false).ok());
    WorldObject moving = lamp;
    moving.references = {anchor.id};
    REQUIRE(editor.add_object(moving, false).ok());
    REQUIRE(editor.save().ok());
    CHECK_FALSE(editor.dirty());

    REQUIRE(editor.move_object(lamp.id, at_cell(3, -2)).ok());

    const auto location = engine::world::find_object(editor.world(), lamp.id);
    REQUIRE(location.has_value());
    const WorldObject& found = engine::world::object_at(editor.world(), *location);
    CHECK(found.id == lamp.id);
    CHECK(found.name == "lamp");
    REQUIRE(found.references.size() == 1);
    CHECK(found.references.front() == anchor.id);
    CHECK(editor.world().cells[location->cell].coord == CellCoord{3, -2});
    CHECK(editor.dirty());
    CHECK(engine::world::validate_world(editor.world()).empty());

    // The cell it left and the cell it entered are dirty; the anchor's cell is untouched.
    std::size_t dirty_cells = 0;
    for (const auto& cell : editor.world().cells) {
        dirty_cells += cell.dirty ? 1u : 0u;
    }
    CHECK(dirty_cells == 2u);
}

TEST_CASE("A saved world reopens with its moved objects, and a save writes only the dirty cells", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    const WorldObject mover = object_named("mover", at_cell(1, 1));
    const WorldObject still = object_named("still", at_cell(7, 7));
    REQUIRE(editor.add_object(mover, false).ok());
    REQUIRE(editor.add_object(still, false).ok());
    REQUIRE(editor.add_object(object_named("sky", glm::vec3(0.0f, 50.0f, 0.0f)), true).ok());
    const auto first = editor.save();
    REQUIRE(first.ok());
    CHECK(first.value() == 3u);  // two cells and the layer
    CHECK_FALSE(editor.dirty());

    REQUIRE(editor.move_object(mover.id, at_cell(-1, 2)).ok());
    const auto written = editor.save();
    REQUIRE(written.ok());
    CHECK(written.value() == 2u);  // the two cells the move touched; the still cell and the layer stay as they were

    auto reopened = WorldEditor::open(path_of(directory));
    REQUIRE(reopened.ok());
    const auto location = engine::world::find_object(reopened.value().world(), mover.id);
    REQUIRE(location.has_value());
    CHECK(engine::world::object_at(reopened.value().world(), *location).name == "mover");
    CHECK(reopened.value().world().cells[location->cell].coord == CellCoord{-1, 2});
    CHECK_FALSE(reopened.value().dirty());
    const auto layer = engine::world::find_object(reopened.value().world(), still.id);
    CHECK(layer.has_value());
}

TEST_CASE("An unloaded cell keeps its edits in memory and in the next save", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    const WorldObject prop = object_named("prop", at_cell(2, 2));
    REQUIRE(editor.add_object(prop, false).ok());
    REQUIRE(editor.save().ok());

    const WorldObject late = object_named("late", at_cell(2, 2));
    REQUIRE(editor.add_object(late, false).ok());
    editor.unload_cell(CellCoord{2, 2});
    REQUIRE(tick_until(editor, [&] { return editor.phase(CellCoord{2, 2}) == CellPhase::Unloaded; }));

    // The streamed copy is gone, but the authored cell still holds the new object and is still dirty.
    CHECK(editor.suppressed(CellCoord{2, 2}));
    CHECK(engine::world::find_object(editor.world(), late.id).has_value());
    CHECK(editor.dirty());

    REQUIRE(editor.save().ok());
    auto reopened = WorldEditor::open(path_of(directory));
    REQUIRE(reopened.ok());
    CHECK(engine::world::find_object(reopened.value().world(), late.id).has_value());
    CHECK(engine::world::find_object(reopened.value().world(), prop.id).has_value());
}

TEST_CASE("Load makes a cell resident outside the camera's ring, and unload clears it", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    REQUIRE(editor.add_object(object_named("far", at_cell(20, 20)), false).ok());
    REQUIRE(editor.add_object(object_named("near", at_cell(0, 0)), false).ok());

    editor.set_camera(glm::dvec2(128.0, 128.0));
    const CellCoord far{20, 20};
    CHECK(editor.phase(far) == CellPhase::Unloaded);
    CHECK(tick_until(editor, [&] { return editor.phase(CellCoord{0, 0}) == CellPhase::Active; }));

    editor.load_cell(far);
    CHECK(editor.pinned(far));
    REQUIRE(tick_until(editor, [&] { return editor.phase(far) != CellPhase::Unloaded; }));

    editor.unload_cell(far);
    CHECK_FALSE(editor.pinned(far));
    CHECK(editor.suppressed(far));
    REQUIRE(tick_until(editor, [&] { return editor.phase(far) == CellPhase::Unloaded; }));
}

TEST_CASE("Navigating to a cell puts the camera at its centre", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    REQUIRE(editor.add_object(object_named("prop", at_cell(-3, 4)), false).ok());

    editor.go_to_cell(CellCoord{-3, 4});
    CHECK(editor.camera().x == Catch::Approx(-3.0 * 256.0 + 128.0));
    CHECK(editor.camera().y == Catch::Approx(4.0 * 256.0 + 128.0));
}

TEST_CASE("A world edit while its cells are resident is seen by the next residency update", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    REQUIRE(editor.add_object(object_named("seed", at_cell(0, 0)), false).ok());
    editor.set_camera(glm::dvec2(128.0, 128.0));
    REQUIRE(tick_until(editor, [&] { return editor.phase(CellCoord{0, 0}) == CellPhase::Active; }));

    // The mover enters the cell the camera can see. The next tick rebuilds the stream cache before it updates residency, so the
    // streamed file of that cell already holds the mover.
    const WorldObject mover = object_named("mover", at_cell(1, 0));
    REQUIRE(editor.add_object(mover, false).ok());
    REQUIRE(editor.move_object(mover.id, at_cell(0, 0)).ok());
    REQUIRE(editor.tick().ok());

    const auto& cell = editor.world().cells[engine::world::find_object(editor.world(), mover.id)->cell];
    const auto stream_file = path_of(directory / ".stream").join(engine::world::cell_file_name(cell));
    REQUIRE(stream_file.ok());
    const auto streamed = engine::core::read_text_file(stream_file.value());
    REQUIRE(streamed.ok());
    CHECK(streamed.value().find("mover") != std::string::npos);
    CHECK(editor.dirty());
}

TEST_CASE("A pin survives the stream rebuild that an edit causes", "[unit][editor]") {
    const fs::path directory = scratch_dir();
    const ScratchGuard guard(directory);
    WorldEditor editor = make_world(directory);
    REQUIRE(editor.add_object(object_named("sky", glm::vec3(0.0f, 60.0f, 0.0f)), true).ok());
    const WorldObject far = object_named("far", at_cell(30, 30));
    REQUIRE(editor.add_object(far, false).ok());

    editor.set_pinned(CellCoord{30, 30}, true);
    REQUIRE(editor.move_object(far.id, at_cell(30, 31)).ok());
    // The edit emptied the pinned cell but did not remove it, so the pin keeps it resident after the rebuild. The mover's new cell is
    // not pinned and stays unloaded.
    REQUIRE(tick_until(editor, [&] { return editor.phase(CellCoord{30, 30}) != CellPhase::Unloaded; }));
    CHECK(editor.pinned(CellCoord{30, 30}));
    CHECK(editor.phase(CellCoord{30, 31}) == CellPhase::Unloaded);
    CHECK(editor.dirty());
}
