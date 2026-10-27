// The editor document model (US-061): startup with a saved or overridden project, invalid projects and damaged workspace files that do not
// stop the editor, saved workspace round trips, the layout reset, and the panel and quit state. No window or graphics device is used.

#include "editor_fixture.hpp"

#include "engine/editor/editor_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using engine::core::LogLevel;
using engine::editor::EditorModel;
using engine::editor::Panel;
using engine::editor::ProjectState;
using engine::tests::editor_fixture::Scratch;
using engine::tests::editor_fixture::write_file;

namespace {

// True when the console holds an entry at the given level whose message contains `text`.
bool console_has(const engine::editor::Console& console, LogLevel level, std::string_view text) {
    for (const auto& entry : console.snapshot()) {
        if (entry.level == level && entry.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Opening a project that is not valid is logged as an error and the editor keeps running", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<engine::editor::Console>();
    EditorModel model(console, scratch.path.join("workspace.json").value());

    const std::string missing = scratch.path.join("missing").value().str();
    CHECK_FALSE(model.open_project(missing).ok());
    CHECK(model.project().state() == ProjectState::Invalid);
    CHECK(console_has(*console, LogLevel::Error, "cannot open project"));
    CHECK(model.workspace().last_project.empty());
    CHECK_FALSE(model.quit_requested());
}

TEST_CASE("Creating a project writes it, opens it, and records it as the last project", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<engine::editor::Console>();
    EditorModel model(console, scratch.path.join("workspace.json").value());

    REQUIRE(model.create_project(scratch.path.str(), "Проект").ok());
    CHECK(model.project().state() == ProjectState::Open);
    CHECK(model.project().info()->name == "Проект");
    CHECK(model.workspace().last_project == scratch.path.join("Проект").value().str());
    CHECK(console_has(*console, LogLevel::Info, "created project"));

    // The same name again fails and keeps the open project.
    CHECK_FALSE(model.create_project(scratch.path.str(), "Проект").ok());
    CHECK(model.project().state() == ProjectState::Open);
    CHECK(console_has(*console, LogLevel::Error, "cannot create project"));
}

TEST_CASE("Startup opens the last project from the workspace, or the one given on the command line", "[unit][editor]") {
    Scratch scratch;
    const auto workspace_file = scratch.path.join("workspace.json").value();
    auto created_root = scratch.path.join("first").value();
    {
        EditorModel writer(std::make_shared<engine::editor::Console>(), workspace_file);
        REQUIRE(writer.create_project(scratch.path.str(), "first").ok());
        REQUIRE(writer.save_workspace().ok());
    }

    auto reopen = std::make_shared<engine::editor::Console>();
    EditorModel reader(reopen, workspace_file);
    reader.startup("");
    CHECK(reader.project().state() == ProjectState::Open);
    CHECK(reader.project().info()->root.str() == created_root.str());

    // A command-line project replaces the saved one for this session.
    Scratch other;
    REQUIRE(engine::editor::create_project(other.path, "second").ok());
    EditorModel override_model(std::make_shared<engine::editor::Console>(), workspace_file);
    override_model.startup(other.path.join("second").value().str());
    CHECK(override_model.project().info()->name == "second");
}

TEST_CASE("A damaged workspace file falls back to the defaults and says why", "[unit][editor]") {
    Scratch scratch;
    write_file(scratch.path, "workspace.json", "{ damaged");
    auto console = std::make_shared<engine::editor::Console>();
    EditorModel model(console, scratch.path.join("workspace.json").value());

    model.startup("");
    CHECK(model.project().state() == ProjectState::Closed);
    CHECK(model.panel_visible(Panel::Console));
    CHECK(console_has(*console, LogLevel::Error, "is not used"));
}

TEST_CASE("Saving the workspace keeps the layout and panel flags for the next start", "[unit][editor]") {
    Scratch scratch;
    const auto workspace_file = scratch.path.join("workspace.json").value();
    {
        EditorModel model(std::make_shared<engine::editor::Console>(), workspace_file);
        model.set_panel_visible(Panel::Viewport, false);
        model.set_panel_visible(Panel::Workspace, true);
        model.workspace().layout = "[Window][Project]\nPos=0,0\n";
        REQUIRE(model.save_workspace().ok());
    }
    EditorModel reloaded(std::make_shared<engine::editor::Console>(), workspace_file);
    reloaded.startup("");
    CHECK_FALSE(reloaded.panel_visible(Panel::Viewport));
    CHECK(reloaded.panel_visible(Panel::Workspace));
    CHECK(reloaded.workspace().layout == "[Window][Project]\nPos=0,0\n");
}

TEST_CASE("Resetting the layout clears the saved layout and is taken once", "[unit][editor]") {
    Scratch scratch;
    EditorModel model(std::make_shared<engine::editor::Console>(), scratch.path.join("workspace.json").value());
    model.workspace().layout = "[Window][Console]\n";

    model.reset_layout();
    CHECK(model.workspace().layout.empty());
    CHECK(model.take_layout_reset());
    CHECK_FALSE(model.take_layout_reset());
}

TEST_CASE("Closing a project leaves no scan behind, and a quit request is recorded", "[unit][editor]") {
    Scratch scratch;
    EditorModel model(std::make_shared<engine::editor::Console>(), scratch.path.join("workspace.json").value());
    REQUIRE(model.create_project(scratch.path.str(), "closing").ok());
    model.close_project();
    CHECK(model.project().state() == ProjectState::Closed);
    CHECK(model.project().scan() == nullptr);

    CHECK_FALSE(model.quit_requested());
    model.request_quit();
    CHECK(model.quit_requested());
}
