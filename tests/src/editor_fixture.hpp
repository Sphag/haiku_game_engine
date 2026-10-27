#pragma once

// Helpers shared by the editor tests (US-061): a scratch folder that is removed at the end of a test, text files written under it,
// and a headless Dear ImGui context whose font has the Cyrillic range. None of them needs a window or a graphics device.

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace engine::tests::editor_fixture {

using engine::core::Guid;
using engine::core::Path;

// A scratch folder under the system temp folder, removed when the object is destroyed.
struct Scratch {
    Path path;

    Scratch() : path(make_path()) {}
    ~Scratch() {
        static_cast<void>(engine::core::remove_all(path));
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

private:
    static Path make_path() {
        auto base = Path::from_utf8(std::filesystem::temp_directory_path().string());
        REQUIRE(base.ok());
        auto dir = base.value().join("engine_editor_tests_" + Guid::generate().to_string());
        REQUIRE(dir.ok());
        REQUIRE(engine::core::create_directories(dir.value()).ok());
        return dir.value();
    }
};

// Writes text to dir/name. dir must exist.
inline void write_file(const Path& dir, std::string_view name, std::string_view text) {
    auto file = dir.join(name);
    REQUIRE(file.ok());
    REQUIRE(engine::core::write_file_atomic(file.value(), text).ok());
}

// A headless ImGui context: no ini file, a font, and the font atlas built so a frame can start. With cyrillic_font the font is the
// Roboto file the editor ships, with the Cyrillic range; if that file cannot be read the built-in font is used instead.
class ImGuiScope {
public:
    explicit ImGuiScope(bool cyrillic_font = false) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // The editor shell docks its panels, so the context needs docking as the host sets it.
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        font_ = cyrillic_font ? io.Fonts->AddFontFromFileTTF(ENGINE_TEST_FONT_PATH, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic()) : nullptr;
        if (font_ == nullptr) {
            font_ = io.Fonts->AddFontDefault();
        }
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiScope() {
        ImGui::DestroyContext();
    }
    ImGuiScope(const ImGuiScope&) = delete;
    ImGuiScope& operator=(const ImGuiScope&) = delete;

    [[nodiscard]] ImFont* font() const noexcept {
        return font_;
    }

private:
    ImFont* font_ = nullptr;
};

}  // namespace engine::tests::editor_fixture
