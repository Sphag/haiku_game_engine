// Dear ImGui in the editor (US-061) without a window or a device: the platform/input adapter writes keys, text, buttons, and the polled
// pointer into the IO queue; the font has Cyrillic glyphs; and a shell frame builds with an invalid project, a missing font, and a
// Cyrillic panel visible. The draw data is checked for geometry, so a frame that built nothing fails here.

#include "editor_fixture.hpp"

#include "engine/editor/editor_model.hpp"
#include "engine/editor/editor_ui.hpp"
#include "engine/editor/imgui_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <memory>
#include <optional>
#include <string>

using engine::editor::EditorModel;
using engine::editor::Panel;
using engine::platform::InputEvent;
using engine::platform::Key;
using engine::platform::WindowId;
using engine::tests::editor_fixture::ImGuiScope;
using engine::tests::editor_fixture::Scratch;

namespace {

InputEvent event(engine::platform::InputEventData data) {
    return InputEvent{WindowId{}, std::move(data)};
}

}  // namespace

TEST_CASE("Platform keys map onto the ImGui keys the editor uses", "[unit][editor]") {
    using engine::editor::to_imgui_key;

    CHECK(to_imgui_key(Key::A) == ImGuiKey_A);
    CHECK(to_imgui_key(Key::Z) == ImGuiKey_Z);
    CHECK(to_imgui_key(Key::Digit0) == ImGuiKey_0);
    CHECK(to_imgui_key(Key::Digit7) == ImGuiKey_7);
    CHECK(to_imgui_key(Key::F1) == ImGuiKey_F1);
    CHECK(to_imgui_key(Key::F12) == ImGuiKey_F12);
    CHECK(to_imgui_key(Key::Enter) == ImGuiKey_Enter);
    CHECK(to_imgui_key(Key::Left) == ImGuiKey_LeftArrow);
    CHECK(to_imgui_key(Key::LeftControl) == ImGuiKey_LeftCtrl);
    CHECK_FALSE(to_imgui_key(Key::Unknown).has_value());
    CHECK_FALSE(to_imgui_key(Key::Count).has_value());
}

TEST_CASE("Typed Cyrillic text reaches the ImGui input queue as code points", "[unit][editor]") {
    ImGuiScope scope;
    engine::editor::handle_imgui_input(event(engine::platform::TextInput{"Щ"}));
    // ImGui queues text as input events and moves them into InputQueueCharacters when the frame starts.
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    REQUIRE(io.InputQueueCharacters.Size == 1);
    CHECK(io.InputQueueCharacters[0] == 0x0429);
    ImGui::EndFrame();
}

TEST_CASE("A pressed key, a button, and the polled pointer reach the frame", "[unit][editor]") {
    ImGuiScope scope;
    engine::editor::begin_imgui_frame(800.0f, 600.0f, 1.0f / 60.0f);
    engine::editor::set_imgui_focused(true);
    engine::editor::set_imgui_pointer(engine::platform::PointerPosition{120.0, 45.0});
    engine::editor::handle_imgui_input(event(engine::platform::MouseButtonPressed{engine::platform::MouseButton::Right}));
    engine::editor::handle_imgui_input(event(engine::platform::KeyPressed{Key::A}));
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    CHECK(io.MousePos.x == 120.0f);
    CHECK(io.MousePos.y == 45.0f);
    CHECK(io.MouseDown[1]);
    CHECK(io.DisplaySize.x == 800.0f);
    CHECK(io.DeltaTime > 0.0f);
    ImGui::EndFrame();
}

TEST_CASE("The Cyrillic font has glyphs for Russian letters", "[unit][editor]") {
    ImGuiScope scope(true);
    ImFont* font = scope.font();
    REQUIRE(font != nullptr);
    CHECK(font->IsGlyphInFont(static_cast<ImWchar>(0x041F)));  // П
    CHECK(font->IsGlyphInFont(static_cast<ImWchar>(0x044F)));  // я
    CHECK(font->IsGlyphInFont(static_cast<ImWchar>(0x0401)));  // Ё
}

TEST_CASE("A shell frame builds with an invalid project and a visible Cyrillic panel", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<engine::editor::Console>();
    EditorModel model(console, scratch.path.join("workspace.json").value());
    model.startup("");
    static_cast<void>(model.open_project(scratch.path.join("absent").value().str()));
    model.set_panel_visible(Panel::Workspace, true);

    ImGuiScope scope(true);
    engine::editor::EditorUi ui(model);
    ui.apply_saved_layout();

    ImGui::NewFrame();
    ui.build();
    ImGui::Render();

    const ImDrawData* draw_data = ImGui::GetDrawData();
    REQUIRE(draw_data != nullptr);
    CHECK(draw_data->Valid);
    CHECK(draw_data->TotalVtxCount > 0);
    CHECK(draw_data->TotalIdxCount > 0);
    // The failed open is in the console, so the Errors panel has something to show.
    CHECK(console->snapshot().size() >= 1);
    CHECK(model.project().state() == engine::editor::ProjectState::Invalid);
}

TEST_CASE("A shell frame builds with every panel hidden and no layout", "[unit][editor]") {
    Scratch scratch;
    EditorModel model(std::make_shared<engine::editor::Console>(), scratch.path.join("workspace.json").value());
    for (const Panel panel : {Panel::Project, Panel::Console, Panel::Errors, Panel::Viewport, Panel::Workspace}) {
        model.set_panel_visible(panel, false);
    }

    ImGuiScope scope;
    engine::editor::EditorUi ui(model);
    ui.apply_saved_layout();
    ImGui::NewFrame();
    ui.build();
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData() != nullptr);
    CHECK(ImGui::GetDrawData()->Valid);
}
