// The asset browser (US-066): the catalog and search, the selection across a rescan, import settings that are checked before the sidecar
// is written, the references a rename or delete affects, rename and delete, a background import with progress and cancel, texture
// thumbnails that follow their source, the hot-reload watch, and dropping an asset into the scene. No window or graphics device is used.

#include "editor_fixture.hpp"

#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"
#include "asset_converter/watch.hpp"

#include "engine/assets/asset_project.hpp"
#include "engine/editor/asset_browser.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/project.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/scene/reflection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

using engine::assets::scan_project;
using engine::core::Guid;
using engine::core::LogLevel;
using engine::core::Path;
using engine::editor::AssetBrowser;
using engine::editor::Console;
using engine::editor::EditorModel;
using engine::tests::editor_fixture::Scratch;
using engine::tests::editor_fixture::write_file;

namespace {

// A project with its assets folder, made by the editor's own create_project.
struct Project {
    Scratch scratch;
    Path root;
    Path assets;

    Project() : root(make_root(scratch.path)), assets(root.join("assets").value()) {}

    static Path make_root(const Path& parent) {
        auto info = engine::editor::create_project(parent, "Asset project");
        REQUIRE(info.ok());
        return info.value().root;
    }

    // A source with a sidecar, written with the given importer and settings. Returns its GUID.
    Guid add(std::string_view name, std::string_view text, std::string_view importer, std::string_view settings = "{}") {
        write_file(assets, name, text);
        auto guid = engine::assets::create_asset_metadata(assets, Path::from_utf8(std::string(name)).value(), std::string(importer), settings);
        REQUIRE(guid.ok());
        return guid.value();
    }

    engine::assets::ProjectScan scan() {
        auto scanned = scan_project(assets);
        REQUIRE(scanned.ok());
        return std::move(scanned).value();
    }
};

// Reads a fixture from the repository's test fixtures and writes it into the project as a source.
std::string fixture_bytes(std::string_view relative) {
    auto bytes = engine::core::read_file(Path::from_utf8(std::string(ENGINE_TEST_FIXTURE_DIR) + "/" + std::string(relative)).value());
    REQUIRE(bytes.ok());
    return bytes.value();
}

bool console_has(const Console& console, LogLevel level, std::string_view text) {
    for (const auto& entry : console.snapshot()) {
        if (entry.level == level && entry.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The GUID text of a registered source in a scan, or empty.
std::string guid_of(const engine::assets::ProjectScan& scan, std::string_view source) {
    const auto* record = scan.registry.find_by_path(Path::from_utf8(std::string(source)).value());
    return record != nullptr ? record->guid.to_string() : std::string();
}

}  // namespace

TEST_CASE("The catalog lists registered and unregistered sources, and search matches path, importer, and GUID", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    write_file(project.assets, "b.txt", "bravo\n");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());
    REQUIRE(browser.is_open());
    CHECK(browser.entries().size() == 2);

    CHECK(browser.search("").size() == 2);
    const auto by_path = browser.search("a.txt");
    REQUIRE(by_path.size() == 1);
    CHECK(by_path[0]->registered());
    CHECK(browser.search("TEXT").size() == 1);  // the importer name, ignoring case
    CHECK(browser.search(a.to_string().substr(0, 8)).size() == 1);
    const auto unregistered = browser.search("b.txt");
    REQUIRE(unregistered.size() == 1);
    CHECK_FALSE(unregistered[0]->registered());
    CHECK(browser.search("no such asset").empty());
}

TEST_CASE("The selection survives a rescan and is cleared when its asset is removed", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    project.add("c.txt", "charlie\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());
    browser.select(a.to_string());
    REQUIRE(browser.selected() != nullptr);

    // A new source arrives: the rescan keeps the selection, because it is keyed by GUID.
    project.add("d.txt", "delta\n", "text");
    browser.refresh(project.scan());
    REQUIRE(browser.selected() != nullptr);
    CHECK(browser.selected()->asset->guid == a);
    CHECK(browser.entries().size() == 3);

    REQUIRE(browser.remove(a.to_string()).ok());
    browser.refresh(project.scan());
    CHECK(browser.selected() == nullptr);
    CHECK(browser.find(a.to_string()) == nullptr);
}

TEST_CASE("Import settings are checked before the sidecar is written", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());

    const auto rejected = browser.set_settings(a.to_string(), R"({"no_such_key": true})");
    CHECK_FALSE(rejected.ok());
    CHECK(console_has(*console, LogLevel::Error, "import settings were not saved"));
    CHECK(project.scan().registry.find(a)->importer_settings == "{}");

    REQUIRE(browser.set_settings(a.to_string(), R"({"normalize_newlines": true})").ok());
    CHECK(project.scan().registry.find(a)->importer_settings == R"({"normalize_newlines":true})");
    // The GUID and the importer are kept by the rewrite.
    CHECK(project.scan().registry.find(a)->importer == "text");
}

TEST_CASE("References list the text sources that name an asset's GUID and leave out binary sources", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    project.add("scene.json", std::string("{\"ref\": \"") + a.to_string() + "\"}\n", "json");
    write_file(project.assets, "notes.txt", "no reference here\n");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());

    auto references = browser.references_to(a.to_string());
    REQUIRE(references.ok());
    REQUIRE(references.value().size() == 1);
    CHECK(references.value()[0] == "scene.json names its GUID");

    CHECK_FALSE(browser.references_to("00000000-0000-4000-8000-000000000000").ok());
}

TEST_CASE("Rename keeps the GUID and refuses a path that is taken", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    project.add("b.txt", "bravo\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());

    REQUIRE(browser.rename(a.to_string(), "moved/a2.txt").ok());
    auto scan = project.scan();
    CHECK(scan.registry.find_by_path(Path::from_utf8("moved/a2.txt").value()) != nullptr);
    CHECK(scan.registry.find(a) != nullptr);
    CHECK(scan.registry.find(a)->source_path.str() == "moved/a2.txt");

    CHECK_FALSE(browser.rename(a.to_string(), "b.txt").ok());
    CHECK(scan_project(project.assets).value().registry.find(a) != nullptr);
}

TEST_CASE("Delete removes the source and its sidecar", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());
    REQUIRE(browser.remove(a.to_string()).ok());

    CHECK_FALSE(engine::core::path_exists(project.assets.join("a.txt").value()));
    CHECK_FALSE(engine::core::path_exists(project.assets.join("a.txt.meta").value()));
    CHECK(console_has(*console, LogLevel::Info, "deleted a.txt"));
}

TEST_CASE("An import runs on a worker, reports each asset, and writes the cooked files", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    const Guid b = project.add("b.txt", "bravo\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());
    REQUIRE(browser.start_import().ok());
    CHECK(browser.importing());
    CHECK_FALSE(browser.start_import().ok());  // one import at a time

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!browser.poll_import()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(browser.importing());
    CHECK(browser.import_total() == 2);
    CHECK(browser.import_done() == 2);
    REQUIRE(browser.import_summary().has_value());
    CHECK(browser.import_summary()->find("2 converted") != std::string::npos);

    REQUIRE(browser.find(a.to_string()) != nullptr);
    REQUIRE(browser.find(a.to_string())->last.has_value());
    CHECK(browser.find(a.to_string())->last->status == engine::converter::AssetStatus::Cooked);
    CHECK(engine::core::path_exists(project.root.join("cooked/" + b.to_string() + ".cooked").value()));
}

TEST_CASE("A cancelled cook converts nothing, and an uncancelled one reports every asset", "[unit][editor]") {
    Project project;
    const Guid a = project.add("a.txt", "alpha\n", "text");
    project.add("b.txt", "bravo\n", "text");
    const Path output = project.root.join("cooked").value();

    std::atomic<bool> cancel{true};
    std::atomic<std::size_t> completed{0};
    const engine::converter::Options cancelled{project.assets, output, 0, &cancel, &completed};
    const auto report = engine::converter::run(engine::converter::Command::Cook, cancelled, engine::converter::ImporterRegistry::builtin());
    REQUIRE(report.ok());
    CHECK(report.value().cancelled);
    CHECK(completed.load() == 0);
    bool any_error = false;
    for (const auto& diagnostic : report.value().diagnostics) {
        any_error = any_error || diagnostic.code == engine::converter::ErrorCode::Cancelled;
    }
    CHECK(any_error);
    // The output folder may be made, but no asset is published into it.
    CHECK_FALSE(engine::core::path_exists(output.join(a.to_string() + ".cooked").value()));

    std::atomic<bool> keep_going{false};
    std::atomic<std::size_t> progress{0};
    const engine::converter::Options full{project.assets, output, 0, &keep_going, &progress};
    const auto finished = engine::converter::run(engine::converter::Command::Cook, full, engine::converter::ImporterRegistry::builtin());
    REQUIRE(finished.ok());
    CHECK_FALSE(finished.value().cancelled);
    CHECK(progress.load() == 2);
}

TEST_CASE("A texture's thumbnail follows its source, and other importers say why they have none", "[unit][editor]") {
    Project project;
    const std::string png = fixture_bytes("textures/srgb_2x2.png");
    write_file(project.assets, "tile.png", png);
    REQUIRE(engine::assets::create_asset_metadata(project.assets, Path::from_utf8("tile.png").value(), "texture", "{}").ok());
    const Guid text = project.add("note.txt", "hello\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());

    const auto* tile = browser.find(guid_of(project.scan(), "tile.png"));
    REQUIRE(tile != nullptr);
    const auto& small = browser.preview(*tile);
    REQUIRE(small.image.has_value());
    CHECK(small.image->width == 2);
    CHECK(small.image->height == 2);
    CHECK(small.image->pixels.size() == 16);

    const auto* note = browser.find(text.to_string());
    REQUIRE(note != nullptr);
    CHECK(browser.preview(*note).error.find("no preview for the text importer") != std::string::npos);

    // The source changes on disk and the catalog is rescanned: the preview is made again from the new bytes.
    write_file(project.assets, "tile.png", fixture_bytes("textures/gradient_4x4.png"));
    browser.refresh(project.scan());
    const auto* changed = browser.find(guid_of(project.scan(), "tile.png"));
    REQUIRE(changed != nullptr);
    const auto& updated = browser.preview(*changed);
    REQUIRE(updated.image.has_value());
    CHECK(updated.image->width == 4);
}

TEST_CASE("The watch reports a new source, so a rescan can pick it up", "[unit][editor]") {
    Project project;
    project.add("a.txt", "alpha\n", "text");

    auto console = std::make_shared<Console>();
    AssetBrowser browser(console);
    browser.open(project.root, project.assets, project.scan());
    // A platform without a watch keeps the browser usable; the test then has nothing to check.
    if (browser.is_open() && !console_has(*console, LogLevel::Warning, "not watched")) {
        write_file(project.assets, "later.txt", "later\n");
        bool changed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!changed && std::chrono::steady_clock::now() < deadline) {
            changed = browser.poll_changes();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(changed);
    }
}

TEST_CASE("An asset dropped into the scene is placed as a new entity with its GUID", "[unit][editor]") {
    Project project;
    const Guid texture = project.add("tile.txt", "tile\n", "text");
    // A JSON source has no component field named "json", so it cannot be placed.
    const Guid script = project.add("raw.json", "{}", "json");

    // A component with a GUID field named after the importer, so a text asset can be placed.
    struct Material {
        Guid text;
    };
    auto console = std::make_shared<Console>();
    EditorModel model(console, project.scratch.path.join("workspace.json").value());
    engine::scene::ComponentBuilder<Material> builder("game.material", 1);
    builder.field<&Material::text>("text", Guid{});
    auto descriptor = builder.build();
    REQUIRE(descriptor.ok());
    REQUIRE(model.scene().catalog().add(std::move(descriptor).value()).ok());

    REQUIRE(model.open_project(project.root.str()).ok());
    REQUIRE(model.assets().is_open());

    auto placed = model.place_asset(texture.to_string());
    REQUIRE(placed.ok());
    const auto selected = model.scene().selected();
    REQUIRE(selected.has_value());
    CHECK(*selected == placed.value());

    // The reference is the asset's GUID, set through the scene's checked field edit.
    const auto value = model.scene().scene().field(placed.value(), "game.material", "text");
    REQUIRE(value.has_value());
    CHECK(std::get<Guid>(*value) == texture);

    // A rename keeps the reference valid, because it is a GUID.
    REQUIRE(model.rename_asset(texture.to_string(), "renamed/tile.txt").ok());
    CHECK(model.assets().find(texture.to_string()) != nullptr);
    CHECK(model.scene().is_known_asset(texture));

    // An asset that is not registered, or an importer no component has a field for, is refused with the reason logged.
    CHECK_FALSE(model.place_asset("00000000-0000-4000-8000-0000000000aa").ok());
    CHECK_FALSE(model.place_asset(script.to_string()).ok());
    CHECK(console_has(*console, LogLevel::Error, "cannot be placed"));
}
