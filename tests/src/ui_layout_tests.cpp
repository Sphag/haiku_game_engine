// Game UI layout (US-058): anchors, uniform UI scaling, and the canvas at several window sizes. The checks are on the CPU, so they run
// on every operating system with no GPU or window.

#include "engine/ui/ui.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

using engine::ui::Anchor;
using engine::ui::Insets;
using engine::ui::Placement;
using engine::ui::Rect;
using engine::ui::Ui;
using engine::ui::UiError;
using engine::ui::UiScaling;

namespace {

Rect rect_of(const Ui& ui, engine::ui::ElementId id) {
    const auto result = ui.rect(id);
    REQUIRE(result.ok());
    return result.value();
}

engine::ui::ElementSpec panel(Placement placement) {
    engine::ui::ElementSpec spec;
    spec.kind = engine::ui::ElementKind::Panel;
    spec.placement = placement;
    return spec;
}

}  // namespace

TEST_CASE("Resolve places an anchored rectangle inside its parent", "[unit]") {
    const Rect parent{0.0f, 0.0f, 1280.0f, 720.0f};

    // The top left sits at the anchor point plus (x, y), so a box inset 20 units from the bottom right starts at (-220, -100).
    const Rect corner = engine::ui::resolve(engine::ui::place(Anchor::BottomRight, -220.0f, -100.0f, 200.0f, 80.0f), parent);
    CHECK(corner.x == 1060.0f);
    CHECK(corner.y == 620.0f);
    CHECK(corner.w == 200.0f);
    CHECK(corner.h == 80.0f);

    const Rect centred = engine::ui::resolve(engine::ui::place(Anchor::Centre, -100.0f, -40.0f, 200.0f, 80.0f), parent);
    CHECK(centred.x == 540.0f);
    CHECK(centred.y == 320.0f);  // the centre is at 360, so the top is 40 above it

    const Rect stretched = engine::ui::resolve(engine::ui::stretch(Insets{10.0f, 20.0f, 30.0f, 40.0f}), parent);
    CHECK(stretched.x == 10.0f);
    CHECK(stretched.y == 20.0f);
    CHECK(stretched.w == 1280.0f - 40.0f);
    CHECK(stretched.h == 720.0f - 60.0f);
}

TEST_CASE("UI scale is the smaller ratio to the reference size, times the user scale", "[unit]") {
    Ui ui;
    ui.set_viewport(1920, 1080);
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(1.5, 1e-6));

    ui.set_viewport(800, 600);
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(0.625, 1e-6));

    ui.set_viewport(2560, 1080);
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(1.5, 1e-6));
    CHECK_THAT(ui.canvas().w, Catch::Matchers::WithinAbs(2560.0 / 1.5, 1e-3));
    CHECK_THAT(ui.canvas().h, Catch::Matchers::WithinAbs(720.0, 1e-3));

    UiScaling user;
    user.user_scale = 2.0f;
    REQUIRE(ui.set_scaling(user).ok());
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(3.0, 1e-6));
}

TEST_CASE("Anchored elements keep their corner at every window size", "[unit]") {
    const std::uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {800, 600}, {2560, 1080}, {640, 360}};
    for (const auto& size : sizes) {
        Ui ui;
        ui.set_viewport(size[0], size[1]);
        auto id = ui.add(engine::ui::kNoElement, panel(engine::ui::place(Anchor::BottomRight, -220.0f, -100.0f, 200.0f, 80.0f)));
        REQUIRE(id.ok());
        const Rect canvas = ui.canvas();
        const Rect r = rect_of(ui, id.value());
        // The box's bottom right sits 20 canvas units inside the canvas corner, whatever the window size.
        CHECK_THAT(r.right(), Catch::Matchers::WithinAbs(static_cast<double>(canvas.w - 20.0f), 1e-3));
        CHECK_THAT(r.bottom(), Catch::Matchers::WithinAbs(static_cast<double>(canvas.h - 20.0f), 1e-3));
        CHECK_THAT(r.w, Catch::Matchers::WithinAbs(200.0, 1e-3));
    }
}

TEST_CASE("Child placement is relative to its parent, and a hidden parent hides its subtree", "[unit]") {
    Ui ui;
    ui.set_viewport(1280, 720);
    auto parent_id = ui.add(engine::ui::kNoElement, panel(engine::ui::place(Anchor::TopLeft, 100.0f, 100.0f, 400.0f, 300.0f)));
    REQUIRE(parent_id.ok());
    auto child_id = ui.add(parent_id.value(), panel(engine::ui::stretch(Insets{20.0f, 20.0f, 20.0f, 20.0f})));
    REQUIRE(child_id.ok());

    const Rect child = rect_of(ui, child_id.value());
    CHECK(child.x == 120.0f);
    CHECK(child.y == 120.0f);
    CHECK(child.w == 360.0f);
    CHECK(child.h == 260.0f);
    CHECK(ui.shown(child_id.value()));

    REQUIRE(ui.set_visible(parent_id.value(), false).ok());
    CHECK_FALSE(ui.shown(parent_id.value()));
    CHECK_FALSE(ui.shown(child_id.value()));
    REQUIRE(ui.set_visible(parent_id.value(), true).ok());
    CHECK(ui.shown(child_id.value()));
}

TEST_CASE("Invalid scaling and placement are rejected and change nothing", "[unit]") {
    Ui ui;
    ui.set_viewport(1280, 720);
    UiScaling bad;
    bad.reference_width = 0.0f;
    const auto scaling = ui.set_scaling(bad);
    REQUIRE_FALSE(scaling.ok());
    CHECK(scaling.error() == UiError::InvalidArgument);
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(1.0, 1e-6));

    Placement nan_placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 10.0f, 10.0f);
    nan_placement.offsets.left = std::numeric_limits<float>::quiet_NaN();
    const auto added = ui.add(engine::ui::kNoElement, panel(nan_placement));
    REQUIRE_FALSE(added.ok());
    CHECK(added.error() == UiError::InvalidArgument);
}

TEST_CASE("Without a viewport the canvas is empty and the scale is one", "[unit]") {
    Ui ui;
    CHECK_THAT(ui.scale(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    CHECK(ui.render().width() == 0);
    CHECK(ui.render().height() == 0);
}
