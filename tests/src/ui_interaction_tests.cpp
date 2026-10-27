// Game UI interaction (US-058): hit testing, focus, navigation, buttons, sliders, input capture, and removal. Events are fed to the
// Ui directly, so the tests run on every operating system with no window.

#include "engine/platform/input_router.hpp"
#include "engine/ui/ui.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <utility>
#include <vector>

using engine::platform::InputEvent;
using engine::platform::Key;
using engine::platform::WindowId;
using engine::ui::Anchor;
using engine::ui::ElementId;
using engine::ui::ElementKind;
using engine::ui::ElementSpec;
using engine::ui::kNoElement;
using engine::ui::Placement;
using engine::ui::Rect;
using engine::ui::Ui;
using engine::ui::UiError;

namespace {

InputEvent key(Key k, bool down = true) {
    if (down) {
        return InputEvent{WindowId{}, engine::platform::KeyPressed{k}};
    }
    return InputEvent{WindowId{}, engine::platform::KeyReleased{k}};
}

InputEvent click(bool down) {
    if (down) {
        return InputEvent{WindowId{}, engine::platform::MouseButtonPressed{engine::platform::MouseButton::Left}};
    }
    return InputEvent{WindowId{}, engine::platform::MouseButtonReleased{engine::platform::MouseButton::Left}};
}

InputEvent pad(engine::platform::GamepadButton button) {
    return InputEvent{WindowId{}, engine::platform::GamepadButtonPressed{0, button}};
}

InputEvent motion(int dx, int dy) {
    return InputEvent{WindowId{}, engine::platform::MouseMoved{dx, dy}};
}

ElementSpec button_at(Placement placement, const std::string& label) {
    ElementSpec spec;
    spec.kind = ElementKind::Button;
    spec.placement = placement;
    spec.label = label;
    return spec;
}

ElementSpec slider_at(Placement placement, float value) {
    ElementSpec spec;
    spec.kind = ElementKind::Slider;
    spec.placement = placement;
    spec.minimum = 0.0f;
    spec.maximum = 1.0f;
    spec.step = 0.25f;
    spec.value = value;
    return spec;
}

// A 1280 x 720 viewport, so the scale is one and canvas units are pixels.
Ui make_ui() {
    Ui ui;
    ui.set_viewport(1280, 720);
    ui.set_active(true);
    return ui;
}

}  // namespace

TEST_CASE("A click on a button runs its callback once on release over the same button", "[unit]") {
    Ui ui = make_ui();
    int clicks = 0;
    ElementSpec spec = button_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 200.0f, 80.0f), "Play");
    spec.on_click = [&clicks] { ++clicks; };
    REQUIRE(ui.add(kNoElement, std::move(spec)).ok());

    ui.set_pointer(150.0f, 130.0f);
    (void)ui.handle({click(true)});
    CHECK(clicks == 0);
    (void)ui.handle({click(false)});
    CHECK(clicks == 1);

    // Pressing on the button and releasing away from it does not activate.
    ui.set_pointer(150.0f, 130.0f);
    (void)ui.handle({click(true)});
    ui.set_pointer(10.0f, 10.0f);
    (void)ui.handle({click(false)});
    CHECK(clicks == 1);
}

TEST_CASE("Hit testing returns the topmost interactive element, and skips hidden or disabled ones", "[unit]") {
    Ui ui = make_ui();
    const auto under = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 300.0f, 300.0f), "under"));
    const auto over = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 100.0f, 100.0f), "over"));
    REQUIRE(under.ok());
    REQUIRE(over.ok());

    CHECK(ui.hit_test(150.0f, 150.0f) == over.value());
    CHECK(ui.hit_test(50.0f, 50.0f) == under.value());
    CHECK(ui.hit_test(500.0f, 500.0f) == kNoElement);

    REQUIRE(ui.set_enabled(over.value(), false).ok());
    CHECK(ui.hit_test(150.0f, 150.0f) == under.value());
    REQUIRE(ui.set_visible(under.value(), false).ok());
    CHECK(ui.hit_test(50.0f, 50.0f) == kNoElement);
}

TEST_CASE("A blocking panel takes the pointer and stops clicks reaching the button behind it", "[unit]") {
    Ui ui = make_ui();
    int clicks = 0;
    ElementSpec spec = button_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 200.0f, 80.0f), "Behind");
    spec.on_click = [&clicks] { ++clicks; };
    REQUIRE(ui.add(kNoElement, std::move(spec)).ok());

    ElementSpec menu;
    menu.kind = ElementKind::Panel;
    menu.placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 400.0f, 400.0f);
    menu.blocks_pointer = true;
    const auto menu_id = ui.add(kNoElement, menu);
    REQUIRE(menu_id.ok());

    ui.set_pointer(150.0f, 130.0f);
    CHECK(ui.hit_test(150.0f, 130.0f) == menu_id.value());
    (void)ui.handle({click(true)});
    (void)ui.handle({click(false)});
    CHECK(clicks == 0);
}

TEST_CASE("Tab and Shift+Tab cycle focus over enabled buttons and sliders in tree order", "[unit]") {
    Ui ui = make_ui();
    const auto a = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 50.0f, 50.0f), "a"));
    const auto b = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 60.0f, 50.0f, 50.0f), "b"));
    const auto c = ui.add(kNoElement, slider_at(engine::ui::place(Anchor::TopLeft, 0.0f, 120.0f, 50.0f, 20.0f), 0.5f));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    REQUIRE(ui.set_enabled(b.value(), false).ok());

    (void)ui.handle({key(Key::Tab)});
    CHECK(ui.focus() == a.value());
    (void)ui.handle({key(Key::Tab)});
    CHECK(ui.focus() == c.value());
    (void)ui.handle({key(Key::Tab)});
    CHECK(ui.focus() == a.value());

    (void)ui.handle({key(Key::LeftShift)});
    (void)ui.handle({key(Key::Tab)});
    CHECK(ui.focus() == c.value());
    (void)ui.handle({key(Key::LeftShift, false)});
    (void)ui.handle({key(Key::Tab)});
    CHECK(ui.focus() == a.value());
}

TEST_CASE("Arrow keys and the D-pad move focus to the nearest element in that direction", "[unit]") {
    Ui ui = make_ui();
    const auto left = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 100.0f, 80.0f, 40.0f), "left"));
    const auto right = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 200.0f, 100.0f, 80.0f, 40.0f), "right"));
    const auto below = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 300.0f, 80.0f, 40.0f), "below"));
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    REQUIRE(below.ok());

    REQUIRE(ui.set_focus(left.value()).ok());
    (void)ui.handle({key(Key::Right)});
    CHECK(ui.focus() == right.value());
    (void)ui.handle({key(Key::Left)});
    CHECK(ui.focus() == left.value());
    (void)ui.handle({key(Key::Down)});
    CHECK(ui.focus() == below.value());
    (void)ui.handle({pad(engine::platform::GamepadButton::DPadUp)});
    CHECK(ui.focus() == left.value());
    (void)ui.handle({pad(engine::platform::GamepadButton::DPadRight)});
    CHECK(ui.focus() == right.value());
}

TEST_CASE("Enter, Space, and gamepad A activate the focused button", "[unit]") {
    Ui ui = make_ui();
    int clicks = 0;
    ElementSpec spec = button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 50.0f, 50.0f), "go");
    spec.on_click = [&clicks] { ++clicks; };
    REQUIRE(ui.add(kNoElement, std::move(spec)).ok());

    (void)ui.handle({key(Key::Tab)});
    (void)ui.handle({key(Key::Enter)});
    CHECK(clicks == 1);
    (void)ui.handle({key(Key::Space)});
    CHECK(clicks == 2);
    (void)ui.handle({pad(engine::platform::GamepadButton::A)});
    CHECK(clicks == 3);
}

TEST_CASE("Sliders change by arrow steps and by drag, clamp to their range, and report each change", "[unit]") {
    Ui ui = make_ui();
    std::vector<float> reported;
    ElementSpec spec = slider_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 200.0f, 20.0f), 0.5f);
    spec.on_change = [&reported](float value) { reported.push_back(value); };
    const auto id = ui.add(kNoElement, std::move(spec));
    REQUIRE(id.ok());

    (void)ui.handle({key(Key::Tab)});
    (void)ui.handle({key(Key::Right)});
    REQUIRE(reported.size() == 1);
    CHECK_THAT(reported.back(), Catch::Matchers::WithinAbs(0.75, 1e-6));
    (void)ui.handle({key(Key::Right)});
    (void)ui.handle({key(Key::Right)});
    (void)ui.handle({key(Key::Right)});
    CHECK_THAT(ui.value(id.value()).value(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    const std::size_t at_max = reported.size();
    (void)ui.handle({key(Key::Right)});  // already at the maximum: nothing changes
    CHECK(reported.size() == at_max);
    (void)ui.handle({key(Key::Left)});
    CHECK_THAT(reported.back(), Catch::Matchers::WithinAbs(0.75, 1e-6));

    // Drag: the pointer at the track's left quarter sets 0.25.
    ui.set_pointer(100.0f + 200.0f * 0.25f, 110.0f);
    (void)ui.handle({click(true)});
    CHECK_THAT(ui.value(id.value()).value(), Catch::Matchers::WithinAbs(0.25, 1e-3));
    (void)ui.handle({motion(100, 0)});
    CHECK_THAT(ui.value(id.value()).value(), Catch::Matchers::WithinAbs(0.75, 1e-3));
    (void)ui.handle({click(false)});
    (void)ui.handle({motion(-400, 0)});
    CHECK_THAT(ui.value(id.value()).value(), Catch::Matchers::WithinAbs(0.75, 1e-3));

    // The host's own set_value does not run the callback.
    const std::size_t before = reported.size();
    REQUIRE(ui.set_value(id.value(), 0.1f).ok());
    CHECK(reported.size() == before);
}

TEST_CASE("Input capture: an inactive UI captures nothing, an active one captures keys and the pointer over it", "[unit]") {
    Ui ui;
    ui.set_viewport(1280, 720);
    REQUIRE(ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 200.0f, 80.0f), "b")).ok());
    ui.set_pointer(150.0f, 130.0f);

    CHECK(ui.handle({key(Key::Tab)}).captured == 0);

    ui.set_active(true);
    const auto over = ui.handle({motion(0, 0)});
    CHECK((over.captured & engine::platform::kCaptureKeyboard) != 0);
    CHECK((over.captured & engine::platform::kCaptureGamepad) != 0);
    CHECK((over.captured & engine::platform::kCaptureMouse) != 0);
    CHECK(over.pointer_over_ui);

    ui.set_pointer(10.0f, 10.0f);
    const auto away = ui.handle({motion(0, 0)});
    CHECK((away.captured & engine::platform::kCaptureMouse) == 0);
    CHECK((away.captured & engine::platform::kCaptureKeyboard) != 0);
    CHECK_FALSE(away.pointer_over_ui);
}

TEST_CASE("The pointer starts at the viewport centre, moves by device counts, and stays inside the viewport", "[unit]") {
    Ui ui = make_ui();
    CHECK(ui.pointer_x() == 640.0f);
    CHECK(ui.pointer_y() == 360.0f);
    (void)ui.handle({motion(-40, 10)});
    CHECK(ui.pointer_x() == 600.0f);
    CHECK(ui.pointer_y() == 370.0f);
    (void)ui.handle({motion(100000, -100000)});
    CHECK(ui.pointer_x() == 1280.0f);
    CHECK(ui.pointer_y() == 0.0f);
}

TEST_CASE("Hit testing follows the UI scale at a larger window", "[unit]") {
    Ui ui;
    ui.set_viewport(1920, 1080);  // scale 1.5
    REQUIRE(ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 200.0f, 80.0f), "b")).ok());

    // Canvas (100..300, 100..180) is pixels (150..450, 150..270).
    CHECK(ui.hit_test(200.0f, 200.0f) != kNoElement);
    CHECK(ui.hit_test(140.0f, 200.0f) == kNoElement);
    CHECK(ui.hit_test(460.0f, 200.0f) == kNoElement);
}

TEST_CASE("Removing an element removes its subtree and clears focus on it", "[unit]") {
    Ui ui = make_ui();
    ElementSpec parent;
    parent.kind = ElementKind::Panel;
    parent.placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 300.0f, 300.0f);
    const auto panel = ui.add(kNoElement, parent);
    REQUIRE(panel.ok());
    const auto child = ui.add(panel.value(), button_at(engine::ui::place(Anchor::TopLeft, 10.0f, 10.0f, 50.0f, 50.0f), "c"));
    REQUIRE(child.ok());
    REQUIRE(ui.set_focus(child.value()).ok());

    REQUIRE(ui.remove(panel.value()).ok());
    CHECK(ui.focus() == kNoElement);
    CHECK_FALSE(ui.shown(child.value()));
    CHECK(ui.rect(child.value()).error() == UiError::UnknownElement);
    CHECK(ui.remove(panel.value()).error() == UiError::UnknownElement);
    CHECK(ui.hit_test(20.0f, 20.0f) == kNoElement);
}

TEST_CASE("A callback that removes its own button is safe, and focus moves off a removed element", "[unit]") {
    Ui ui = make_ui();
    ElementId self = kNoElement;
    int clicks = 0;
    ElementSpec spec = button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 50.0f, 50.0f), "once");
    spec.on_click = [&ui, &self, &clicks] {
        ++clicks;
        (void)ui.remove(self);
        // Adding while the callback runs reallocates the element storage; the activation must not use the old storage.
        for (int i = 0; i < 64; ++i) {
            (void)ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 1.0f, 1.0f), "filler"));
        }
    };
    const auto id = ui.add(kNoElement, std::move(spec));
    REQUIRE(id.ok());
    self = id.value();

    (void)ui.handle({key(Key::Tab)});
    (void)ui.handle({key(Key::Enter)});
    CHECK(clicks == 1);
    CHECK(ui.focus() == kNoElement);
    CHECK_FALSE(ui.shown(self));
}

TEST_CASE("Images are registered by unique name and checked when an element uses them", "[unit]") {
    Ui ui = make_ui();
    REQUIRE(ui.add_image("icon", engine::ui::Surface(4, 4)).ok());
    CHECK(ui.add_image("icon", engine::ui::Surface(2, 2)).error() == UiError::DuplicateImage);

    ElementSpec picture;
    picture.kind = ElementKind::Image;
    picture.placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 32.0f, 32.0f);
    picture.image = "missing";
    CHECK(ui.add(kNoElement, picture).error() == UiError::UnknownImage);
    picture.image = "icon";
    CHECK(ui.add(kNoElement, picture).ok());
}

TEST_CASE("Only panels take children, and an invalid slider range is rejected", "[unit]") {
    Ui ui = make_ui();
    const auto button = ui.add(kNoElement, button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 50.0f, 50.0f), "b"));
    REQUIRE(button.ok());
    CHECK(ui.add(button.value(), button_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 5.0f, 5.0f), "c")).error() == UiError::InvalidParent);

    ElementSpec inverted = slider_at(engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 50.0f, 10.0f), 0.0f);
    inverted.minimum = 1.0f;
    inverted.maximum = 0.0f;
    CHECK(ui.add(kNoElement, inverted).error() == UiError::InvalidArgument);
    CHECK(ui.set_focus(button.value()).ok());
}

TEST_CASE("Render output has the viewport size and paints panels and buttons where the layout puts them", "[unit]") {
    const std::uint32_t sizes[][2] = {{640, 360}, {1280, 720}, {800, 600}};
    for (const auto& size : sizes) {
        Ui ui;
        ui.set_viewport(size[0], size[1]);
        ElementSpec panel;
        panel.kind = ElementKind::Panel;
        panel.placement = engine::ui::stretch(engine::ui::Insets{16.0f, 16.0f, 16.0f, 16.0f});
        panel.colour = engine::ui::Rgba8{10, 200, 30, 255};
        REQUIRE(ui.add(kNoElement, panel).ok());

        const engine::ui::Surface image = ui.render();
        REQUIRE(image.width() == size[0]);
        REQUIRE(image.height() == size[1]);
        // The margin is 16 canvas units, so the panel starts at 16 * scale pixels.
        const auto edge = static_cast<std::uint32_t>(16.0f * ui.scale() + 0.5f);
        const engine::ui::Rgba8 inside = image.at(edge + 1, edge + 1);
        CHECK(inside.g == 200);
        CHECK(inside.a == 255);
        CHECK(image.at(edge - 2, edge + 1).a == 0);
    }
}
