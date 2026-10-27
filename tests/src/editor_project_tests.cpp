// Editor projects (US-061): name rules, creating and reading project.json, and the session states an invalid project or a missing
// assets folder leaves behind. The tests write real folders under the system temp folder.

#include "editor_fixture.hpp"

#include "engine/core/file_io.hpp"
#include "engine/editor/project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::core::Path;
using engine::editor::ProjectErrorCode;
using engine::editor::ProjectState;
using engine::editor::ProjectSession;
using engine::tests::editor_fixture::Scratch;
using engine::tests::editor_fixture::write_file;

namespace {

// A project file with the given text, in a fresh folder named "project" under the scratch folder.
Path project_with(const Scratch& scratch, std::string_view text) {
    auto root = scratch.path.join("project");
    REQUIRE(root.ok());
    REQUIRE(engine::core::create_directories(root.value()).ok());
    write_file(root.value(), engine::editor::kProjectFileName, text);
    return root.value();
}

}  // namespace

TEST_CASE("Project names accept Cyrillic and reject names that cannot be a folder", "[unit][editor]") {
    using engine::editor::is_valid_project_name;

    CHECK(is_valid_project_name("Проект"));
    CHECK(is_valid_project_name("demo game 2"));
    CHECK_FALSE(is_valid_project_name(""));
    CHECK_FALSE(is_valid_project_name("."));
    CHECK_FALSE(is_valid_project_name(".."));
    CHECK_FALSE(is_valid_project_name("a/b"));
    CHECK_FALSE(is_valid_project_name("a\\b"));
    CHECK_FALSE(is_valid_project_name("a:b"));
    CHECK_FALSE(is_valid_project_name("a*b"));
    CHECK_FALSE(is_valid_project_name(std::string("a") + '\x01' + "b"));
    CHECK_FALSE(is_valid_project_name("trailing."));
    CHECK_FALSE(is_valid_project_name("trailing "));
    CHECK_FALSE(is_valid_project_name(std::string(129, 'x')));
    CHECK(is_valid_project_name(std::string(128, 'x')));
    CHECK_FALSE(is_valid_project_name("bad\xFF"));
}

TEST_CASE("Creating a project writes project.json and an assets folder that reads back", "[unit][editor]") {
    Scratch scratch;
    const auto created = engine::editor::create_project(scratch.path, "Проект");
    REQUIRE(created.ok());
    CHECK(created.value().name == "Проект");

    const auto read = engine::editor::read_project(created.value().root);
    REQUIRE(read.ok());
    CHECK(read.value().name == "Проект");
    CHECK(engine::core::path_exists(created.value().root.join(engine::editor::kProjectAssetsFolder).value()));
}

TEST_CASE("Creating a project over an existing folder fails and leaves that folder alone", "[unit][editor]") {
    Scratch scratch;
    REQUIRE(engine::core::create_directories(scratch.path.join("taken").value()).ok());
    write_file(scratch.path.join("taken").value(), "keep.txt", "mine");

    const auto created = engine::editor::create_project(scratch.path, "taken");
    REQUIRE_FALSE(created.ok());
    CHECK(created.error().code == ProjectErrorCode::AlreadyExists);
    CHECK(engine::core::path_exists(scratch.path.join("taken").value().join("keep.txt").value()));
    CHECK_FALSE(engine::core::path_exists(scratch.path.join("taken").value().join(engine::editor::kProjectFileName).value()));
}

TEST_CASE("Creating a project needs an existing parent and a valid name", "[unit][editor]") {
    Scratch scratch;
    const auto missing_parent = engine::editor::create_project(scratch.path.join("absent").value(), "game");
    REQUIRE_FALSE(missing_parent.ok());
    CHECK(missing_parent.error().code == ProjectErrorCode::Io);

    const auto bad_name = engine::editor::create_project(scratch.path, "a/b");
    REQUIRE_FALSE(bad_name.ok());
    CHECK(bad_name.error().code == ProjectErrorCode::InvalidName);
    CHECK_FALSE(engine::core::path_exists(scratch.path.join("a").value()));
}

TEST_CASE("A project file that is not a valid engine.project is rejected with its reason", "[unit][editor]") {
    struct Case {
        std::string_view text;
        ProjectErrorCode code;
    };
    const Case cases[] = {
        {"not json", ProjectErrorCode::Malformed},
        {"[1, 2]", ProjectErrorCode::Malformed},
        {R"({"format":"engine.project","version":1})", ProjectErrorCode::Malformed},
        {R"({"format":"engine.project","version":1,"name":"a","extra":true})", ProjectErrorCode::Malformed},
        {R"({"format":"other","version":1,"name":"a"})", ProjectErrorCode::WrongFormat},
        {R"({"format":"engine.project","version":2,"name":"a"})", ProjectErrorCode::UnsupportedVersion},
        {R"({"format":"engine.project","version":"1","name":"a"})", ProjectErrorCode::Malformed},
        {R"({"format":"engine.project","version":1,"name":7})", ProjectErrorCode::Malformed},
        {R"({"format":"engine.project","version":1,"name":"a/b"})", ProjectErrorCode::InvalidName},
    };
    for (const Case& test : cases) {
        Scratch scratch;
        const Path root = project_with(scratch, test.text);
        const auto read = engine::editor::read_project(root);
        INFO(std::string(test.text));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().code == test.code);
    }
}

TEST_CASE("A folder without project.json is an Io error, not a crash", "[unit][editor]") {
    Scratch scratch;
    const auto read = engine::editor::read_project(scratch.path);
    REQUIRE_FALSE(read.ok());
    CHECK(read.error().code == ProjectErrorCode::Io);
}

TEST_CASE("An invalid project leaves the session Invalid with its error and no scan", "[unit][editor]") {
    Scratch scratch;
    const Path root = project_with(scratch, "{");
    ProjectSession session;
    const auto opened = session.open(root);
    REQUIRE_FALSE(opened.ok());
    CHECK(session.state() == ProjectState::Invalid);
    REQUIRE(session.error().has_value());
    CHECK(session.error()->code == ProjectErrorCode::Malformed);
    CHECK(session.scan() == nullptr);
    CHECK_FALSE(session.info().has_value());
}

TEST_CASE("An open project scans its assets and lists unregistered sources and orphaned metadata", "[unit][editor]") {
    Scratch scratch;
    const auto created = engine::editor::create_project(scratch.path, "demo");
    REQUIRE(created.ok());
    const Path assets = created.value().root.join(engine::editor::kProjectAssetsFolder).value();
    // A source with no sidecar, and a sidecar whose source is missing.
    write_file(assets, "texture.png", "pixels");
    write_file(assets, "orphan.png.meta",
               R"({"format":"engine.asset_meta","version":1,"guid":"6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60","importer":{"type":"texture","settings":{}},"dependencies":[]})");

    ProjectSession session;
    REQUIRE(session.open(created.value().root).ok());
    CHECK(session.state() == ProjectState::Open);
    CHECK_FALSE(session.scan_error().has_value());
    REQUIRE(session.scan() != nullptr);
    CHECK(session.scan()->unregistered == std::vector<std::string>{"texture.png"});
    CHECK(session.scan()->orphan_metadata == std::vector<std::string>{"orphan.png.meta"});
}

TEST_CASE("A project without an assets folder is open and reports the missing folder", "[unit][editor]") {
    Scratch scratch;
    const Path root = project_with(scratch, R"({"format":"engine.project","version":1,"name":"bare"})");
    ProjectSession session;
    REQUIRE(session.open(root).ok());
    CHECK(session.state() == ProjectState::Open);
    CHECK(session.scan_error().has_value());
    REQUIRE(session.scan() != nullptr);
    CHECK(session.scan()->registry.size() == 0);

    session.close();
    CHECK(session.state() == ProjectState::Closed);
    CHECK(session.scan() == nullptr);
}
