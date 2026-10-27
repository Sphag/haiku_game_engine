#include "engine/runtime/game_hud.hpp"

#include "engine/script/script_ui.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui_layout.hpp"

extern "C" {
#include "lua.h"
}

#include <utility>

namespace engine::runtime {

GameHud::GameHud() : ui_(std::make_unique<ui::Ui>()) {}

GameHud::~GameHud() = default;

core::Result<std::unique_ptr<GameHud>, std::string> GameHud::create(script::ScriptEngine& scripts, const std::string& layout_text,
                                                                    const std::string& script_text,
                                                                    const std::vector<std::uint8_t>& font_bytes, std::uint32_t width,
                                                                    std::uint32_t height) {
    auto layout = ui::parse_ui_layout(layout_text);
    if (!layout.ok()) {
        return core::failure("the UI layout: " + layout.error());
    }
    auto font = ui::Font::load(font_bytes);
    if (!font.ok()) {
        return core::failure(std::string("the UI font cannot be read"));
    }
    if (!script_text.empty()) {
        if (auto ran = scripts.run("ui", script_text); !ran.ok()) {
            return core::failure("the UI script: " + ran.error().message);
        }
    }

    std::unique_ptr<GameHud> hud(new GameHud());
    ui::Ui& ui = *hud->ui_;
    ui.set_font(std::move(font).value());
    ui::UiScaling scaling;
    scaling.reference_width = layout.value().reference_width;
    scaling.reference_height = layout.value().reference_height;
    if (!ui.set_scaling(scaling).ok()) {
        return core::failure(std::string("the UI reference size is not valid"));
    }
    ui.set_viewport(width, height);
    ui.set_active(true);

    lua_State* state = scripts.state();
    for (ui::LayoutElement& element : layout.value().elements) {
        // A callback is a global Lua function the canvas's script defined.
        const auto bind = [&](const std::string& name, auto make) -> core::Result<core::Unit, std::string> {
            lua_getglobal(state, name.c_str());
            if (!lua_isfunction(state, -1)) {
                lua_pop(state, 1);
                return core::failure("the layout names the function '" + name + "', which the UI script does not define");
            }
            make();
            lua_pop(state, 1);
            return core::success();
        };
        if (!element.on_click.empty()) {
            if (auto bound = bind(element.on_click, [&] { element.spec.on_click = script::ui_action(scripts, -1); }); !bound.ok()) {
                return core::failure(bound.error());
            }
        }
        if (!element.on_change.empty()) {
            if (auto bound = bind(element.on_change, [&] { element.spec.on_change = script::ui_value_action(scripts, -1); }); !bound.ok()) {
                return core::failure(bound.error());
            }
        }
        if (auto added = ui.add(ui::kNoElement, std::move(element.spec)); !added.ok()) {
            return core::failure(std::string("a UI element could not be added"));
        }
        ++hud->elements_;
    }
    return core::success(std::move(hud));
}

void GameHud::update(float pointer_x, float pointer_y, const std::vector<platform::InputEvent>& events) {
    ui_->set_pointer(pointer_x, pointer_y);
    static_cast<void>(ui_->handle(events));
}

ui::Surface GameHud::render() const {
    return ui_->render();
}

}  // namespace engine::runtime
