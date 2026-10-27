// The saved workspace (US-061): the strict JSON format, the round trip of the docking layout and panel flags, the defaults for a missing
// file, and the errors a damaged file gives.

#include "editor_fixture.hpp"

#include "engine/editor/workspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::editor::PanelVisibility;
using engine::editor::WorkspaceErrorCode;
using engine::editor::WorkspaceSettings;
using engine::tests::editor_fixture::Scratch;
using engine::tests::editor_fixture::write_file;

namespace {

WorkspaceSettings sample_settings() {
    WorkspaceSettings settings;
    settings.last_project = "D:/games/Проект demo";
    settings.layout = "[Window][Console]\nPos=0,0\nSize=640,200\n";
    settings.panels.workspace = true;
    settings.panels.viewport = false;
    return settings;
}

}  // namespace

TEST_CASE("A workspace round trips through its text with the layout and panel flags intact", "[unit][editor]") {
    const WorkspaceSettings original = sample_settings();
    const auto parsed = engine::editor::parse_workspace(engine::editor::serialize_workspace(original));
    REQUIRE(parsed.ok());
    CHECK(parsed.value() == original);
}

TEST_CASE("A workspace file that is damaged or from another format is rejected with its reason", "[unit][editor]") {
    struct Case {
        std::string text;
        WorkspaceErrorCode code;
    };
    const std::string good_panels =
        R"("panels":{"project":true,"console":true,"errors":true,"viewport":true,"workspace":false})";
    const Case cases[] = {
        {"[]", WorkspaceErrorCode::Malformed},
        {"nope", WorkspaceErrorCode::Malformed},
        {R"({"format":"engine.editor_workspace","version":1,"last_project":"","layout":"")"
         R"(,)" + good_panels + R"(,"extra":1})",
         WorkspaceErrorCode::Malformed},
        {R"({"format":"engine.editor_workspace","version":1,"last_project":"","layout":""})", WorkspaceErrorCode::Malformed},
        {R"({"format":"other","version":1,"last_project":"","layout":"",)" + good_panels + "}", WorkspaceErrorCode::WrongFormat},
        {R"({"format":"engine.editor_workspace","version":2,"last_project":"","layout":"",)" + good_panels + "}",
         WorkspaceErrorCode::UnsupportedVersion},
        {R"({"format":"engine.editor_workspace","version":1,"last_project":3,"layout":"",)" + good_panels + "}",
         WorkspaceErrorCode::Malformed},
        {R"({"format":"engine.editor_workspace","version":1,"last_project":"","layout":"","panels":{"project":1,"console":true,)"
         R"("errors":true,"viewport":true,"workspace":false}})",
         WorkspaceErrorCode::Malformed},
    };
    for (const Case& test : cases) {
        INFO(test.text);
        const auto parsed = engine::editor::parse_workspace(test.text);
        REQUIRE_FALSE(parsed.ok());
        CHECK(parsed.error().code == test.code);
    }
}

TEST_CASE("A missing workspace file loads as the defaults and a saved one loads back", "[unit][editor]") {
    Scratch scratch;
    const auto file = scratch.path.join("workspace.json");
    REQUIRE(file.ok());

    const auto missing = engine::editor::load_workspace(file.value());
    REQUIRE(missing.ok());
    CHECK(missing.value() == WorkspaceSettings{});

    REQUIRE(engine::editor::save_workspace(file.value(), sample_settings()).ok());
    const auto loaded = engine::editor::load_workspace(file.value());
    REQUIRE(loaded.ok());
    CHECK(loaded.value() == sample_settings());
}

TEST_CASE("A damaged workspace file is an error, and saving into a missing folder reports Io", "[unit][editor]") {
    Scratch scratch;
    write_file(scratch.path, "workspace.json", "{");
    const auto damaged = engine::editor::load_workspace(scratch.path.join("workspace.json").value());
    REQUIRE_FALSE(damaged.ok());
    CHECK(damaged.error().code == WorkspaceErrorCode::Malformed);

    const auto saved = engine::editor::save_workspace(scratch.path.join("absent").value().join("workspace.json").value(), sample_settings());
    REQUIRE_FALSE(saved.ok());
    CHECK(saved.error().code == WorkspaceErrorCode::Io);
}

TEST_CASE("Panel visibility defaults show the shell panels and hide the workspace panel", "[unit][editor]") {
    const PanelVisibility defaults;
    CHECK(defaults.project);
    CHECK(defaults.console);
    CHECK(defaults.errors);
    CHECK(defaults.viewport);
    CHECK_FALSE(defaults.workspace);
}
