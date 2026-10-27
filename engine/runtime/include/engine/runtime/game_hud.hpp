#pragma once

// GameHud: a UI canvas as the game shows it. The layout asset (engine.ui_layout) makes the elements, the canvas's Lua script supplies the
// functions that buttons and sliders name, and the mouse drives it. render() gives a viewport-sized RGBA surface that the renderer blends
// over the picture.

#include "engine/core/result.hpp"
#include "engine/platform/input.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/ui/ui.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::runtime {

class GameHud {
public:
    // The layout text and the script source are the importer payloads of the assets; the font is a TrueType file. A callback the layout names
    // that the script does not define is an error, so a typo shows up at start and not as a button that does nothing.
    [[nodiscard]] static core::Result<std::unique_ptr<GameHud>, std::string> create(script::ScriptEngine& scripts, const std::string& layout_text,
                                                                                    const std::string& script_text,
                                                                                    const std::vector<std::uint8_t>& font_bytes,
                                                                                    std::uint32_t width, std::uint32_t height);
    ~GameHud();
    GameHud(const GameHud&) = delete;
    GameHud& operator=(const GameHud&) = delete;

    // Moves the pointer to a viewport position in pixels, then gives the UI this frame's input events.
    void update(float pointer_x, float pointer_y, const std::vector<platform::InputEvent>& events);

    [[nodiscard]] ui::Surface render() const;
    [[nodiscard]] const ui::Ui& ui() const noexcept {
        return *ui_;
    }
    [[nodiscard]] std::size_t element_count() const noexcept {
        return elements_;
    }

private:
    GameHud();
    std::unique_ptr<ui::Ui> ui_;
    std::size_t elements_ = 0;
};

}  // namespace engine::runtime
