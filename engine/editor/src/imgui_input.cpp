#include "engine/editor/imgui_input.hpp"

#include <cfloat>
#include <variant>

namespace engine::editor {

namespace {

// Maps a contiguous run of platform keys onto a contiguous run of ImGui keys (letters, digits, function keys).
std::optional<ImGuiKey> map_run(int value, int first_platform, int count, ImGuiKey first_imgui) {
    if (value < first_platform || value >= first_platform + count) {
        return std::nullopt;
    }
    return static_cast<ImGuiKey>(static_cast<int>(first_imgui) + (value - first_platform));
}

// Dear ImGui reads io.KeyCtrl and the other modifier flags from the ImGuiMod_* keys, not from the left and right keys, so a backend sends
// both. A modifier is down while either of its two keys is.
struct ModifierState {
    ImGuiContext* context = nullptr;
    bool ctrl[2] = {};
    bool shift[2] = {};
    bool alt[2] = {};
    bool super[2] = {};
};

ModifierState& modifiers() {
    static ModifierState state;
    if (state.context != ImGui::GetCurrentContext()) {
        state = ModifierState{};
        state.context = ImGui::GetCurrentContext();
    }
    return state;
}

void send_modifier(ImGuiKey key, bool down) {
    ModifierState& state = modifiers();
    ImGuiIO& io = ImGui::GetIO();
    const auto update = [&io](bool (&held)[2], ImGuiKey left, ImGuiKey key_, ImGuiKey mod, bool is_down) {
        held[key_ == left ? 0 : 1] = is_down;
        io.AddKeyEvent(mod, held[0] || held[1]);
    };
    switch (key) {
        case ImGuiKey_LeftCtrl:
        case ImGuiKey_RightCtrl:
            update(state.ctrl, ImGuiKey_LeftCtrl, key, ImGuiMod_Ctrl, down);
            break;
        case ImGuiKey_LeftShift:
        case ImGuiKey_RightShift:
            update(state.shift, ImGuiKey_LeftShift, key, ImGuiMod_Shift, down);
            break;
        case ImGuiKey_LeftAlt:
        case ImGuiKey_RightAlt:
            update(state.alt, ImGuiKey_LeftAlt, key, ImGuiMod_Alt, down);
            break;
        case ImGuiKey_LeftSuper:
        case ImGuiKey_RightSuper:
            update(state.super, ImGuiKey_LeftSuper, key, ImGuiMod_Super, down);
            break;
        default:
            break;
    }
}

}  // namespace

std::optional<ImGuiKey> to_imgui_key(platform::Key key) noexcept {
    const int value = static_cast<int>(key);
    if (auto letter = map_run(value, static_cast<int>(platform::Key::A), 26, ImGuiKey_A)) {
        return letter;
    }
    if (auto digit = map_run(value, static_cast<int>(platform::Key::Digit0), 10, ImGuiKey_0)) {
        return digit;
    }
    if (auto function = map_run(value, static_cast<int>(platform::Key::F1), 12, ImGuiKey_F1)) {
        return function;
    }
    switch (key) {
        case platform::Key::Escape:
            return ImGuiKey_Escape;
        case platform::Key::Tab:
            return ImGuiKey_Tab;
        case platform::Key::Space:
            return ImGuiKey_Space;
        case platform::Key::Enter:
            return ImGuiKey_Enter;
        case platform::Key::Backspace:
            return ImGuiKey_Backspace;
        case platform::Key::Insert:
            return ImGuiKey_Insert;
        case platform::Key::Delete:
            return ImGuiKey_Delete;
        case platform::Key::Home:
            return ImGuiKey_Home;
        case platform::Key::End:
            return ImGuiKey_End;
        case platform::Key::PageUp:
            return ImGuiKey_PageUp;
        case platform::Key::PageDown:
            return ImGuiKey_PageDown;
        case platform::Key::Left:
            return ImGuiKey_LeftArrow;
        case platform::Key::Right:
            return ImGuiKey_RightArrow;
        case platform::Key::Up:
            return ImGuiKey_UpArrow;
        case platform::Key::Down:
            return ImGuiKey_DownArrow;
        case platform::Key::LeftShift:
            return ImGuiKey_LeftShift;
        case platform::Key::RightShift:
            return ImGuiKey_RightShift;
        case platform::Key::LeftControl:
            return ImGuiKey_LeftCtrl;
        case platform::Key::RightControl:
            return ImGuiKey_RightCtrl;
        case platform::Key::LeftAlt:
            return ImGuiKey_LeftAlt;
        case platform::Key::RightAlt:
            return ImGuiKey_RightAlt;
        case platform::Key::LeftSuper:
            return ImGuiKey_LeftSuper;
        case platform::Key::RightSuper:
            return ImGuiKey_RightSuper;
        default:
            return std::nullopt;  // Unknown, and the keys the editor does not bind
    }
}

void handle_imgui_input(const platform::InputEvent& event) {
    ImGuiIO& io = ImGui::GetIO();
    if (const auto* pressed = std::get_if<platform::KeyPressed>(&event.data)) {
        if (const auto key = to_imgui_key(pressed->key)) {
            io.AddKeyEvent(*key, true);
            send_modifier(*key, true);
        }
    } else if (const auto* released = std::get_if<platform::KeyReleased>(&event.data)) {
        if (const auto key = to_imgui_key(released->key)) {
            io.AddKeyEvent(*key, false);
            send_modifier(*key, false);
        }
    } else if (const auto* text = std::get_if<platform::TextInput>(&event.data)) {
        io.AddInputCharactersUTF8(text->utf8.c_str());
    } else if (const auto* button = std::get_if<platform::MouseButtonPressed>(&event.data)) {
        io.AddMouseButtonEvent(static_cast<int>(button->button), true);
    } else if (const auto* released_button = std::get_if<platform::MouseButtonReleased>(&event.data)) {
        io.AddMouseButtonEvent(static_cast<int>(released_button->button), false);
    } else if (const auto* wheel = std::get_if<platform::MouseWheelScrolled>(&event.data)) {
        io.AddMouseWheelEvent(static_cast<float>(wheel->notches_x), static_cast<float>(wheel->notches_y));
    }
}

void set_imgui_pointer(std::optional<platform::PointerPosition> position) {
    ImGuiIO& io = ImGui::GetIO();
    if (position) {
        io.AddMousePosEvent(static_cast<float>(position->x), static_cast<float>(position->y));
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
}

void set_imgui_focused(bool focused) {
    if (!focused) {
        modifiers() = ModifierState{};  // the platform releases held keys on focus loss; so does ImGui
        modifiers().context = ImGui::GetCurrentContext();
    }
    ImGui::GetIO().AddFocusEvent(focused);
}

void begin_imgui_frame(float display_width, float display_height, float delta_seconds) {
    ImGuiIO& io = ImGui::GetIO();
    // The display is in framebuffer pixels, so the draw data needs no further scale. ImGui asserts a positive frame time.
    io.DisplaySize = ImVec2(display_width, display_height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = delta_seconds > 0.0f ? delta_seconds : 1.0f / 60.0f;
}

}  // namespace engine::editor
