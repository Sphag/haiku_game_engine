#include "engine/platform/dpi.hpp"
#include "engine/platform/event_queue.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/slot_table.hpp"
#include "engine/platform/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

using engine::platform::CloseRequested;
using engine::platform::EventQueue;
using engine::platform::FramebufferSize;
using engine::platform::LogicalSize;
using engine::platform::Platform;
using engine::platform::PlatformErrorCode;
using engine::platform::SlotTable;
using engine::platform::WindowDesc;
using engine::platform::WindowDpiChanged;
using engine::platform::WindowEvent;
using engine::platform::WindowFocusChanged;
using engine::platform::WindowId;
using engine::platform::WindowMinimized;
using engine::platform::WindowResized;
using engine::platform::WindowRestored;
using engine::platform::dpi_scale_from_dpi;
using engine::platform::framebuffer_from_logical;
using engine::platform::logical_from_framebuffer;
using engine::platform::to_string;
using engine::platform::validate_window_desc;

namespace {

WindowEvent resized(WindowId id, std::uint32_t width, std::uint32_t height) {
    const FramebufferSize framebuffer{width, height};
    return WindowEvent{id, WindowResized{engine::platform::logical_from_framebuffer(framebuffer, 1.0), framebuffer}};
}

}  // namespace

TEST_CASE("Slot table ids resolve only while their value lives", "[unit]") {
    SlotTable<int> table;
    const WindowId id = table.insert(7);

    REQUIRE(id.valid());
    REQUIRE(table.size() == 1);
    REQUIRE(table.find(id) != nullptr);
    CHECK(*table.find(id) == 7);

    CHECK(table.erase(id));
    CHECK(table.find(id) == nullptr);
    CHECK_FALSE(table.erase(id));
    CHECK(table.size() == 0);
}

TEST_CASE("A reused slot issues a new generation and rejects the old id", "[unit]") {
    SlotTable<int> table;
    const WindowId first = table.insert(1);
    REQUIRE(table.erase(first));

    const WindowId second = table.insert(2);
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    CHECK(table.find(first) == nullptr);
    REQUIRE(table.find(second) != nullptr);
    CHECK(*table.find(second) == 2);
}

TEST_CASE("Default and out-of-range ids never resolve", "[unit]") {
    SlotTable<int> table;
    static_cast<void>(table.insert(3));

    CHECK_FALSE(WindowId{}.valid());
    CHECK(table.find(WindowId{}) == nullptr);
    CHECK(table.find(WindowId{99, 1}) == nullptr);
    CHECK_FALSE(table.erase(WindowId{99, 1}));
}

TEST_CASE("Ids stay valid while the table grows", "[unit]") {
    SlotTable<int> table;
    std::vector<WindowId> ids;
    for (int value = 0; value < 200; ++value) {
        ids.push_back(table.insert(value));
    }
    for (int value = 0; value < 200; ++value) {
        const int* found = table.find(ids[static_cast<std::size_t>(value)]);
        REQUIRE(found != nullptr);
        CHECK(*found == value);
    }
    CHECK(table.size() == 200);
}

TEST_CASE("for_each visits only live values", "[unit]") {
    SlotTable<int> table;
    const WindowId a = table.insert(1);
    static_cast<void>(table.insert(2));
    static_cast<void>(table.insert(3));
    REQUIRE(table.erase(a));

    int sum = 0;
    std::size_t visits = 0;
    table.for_each([&](WindowId, int& value) {
        sum += value;
        ++visits;
    });
    CHECK(visits == 2);
    CHECK(sum == 5);
}

TEST_CASE("DPI scale is 1 at 96 and treats unknown DPI as 100 percent", "[unit]") {
    CHECK(dpi_scale_from_dpi(96) == 1.0);
    CHECK(dpi_scale_from_dpi(0) == 1.0);
    CHECK(dpi_scale_from_dpi(120) == 1.25);
    CHECK(dpi_scale_from_dpi(144) == 1.5);
    CHECK(dpi_scale_from_dpi(192) == 2.0);
}

TEST_CASE("Logical and framebuffer sizes are distinct and convert with rounding", "[unit]") {
    const FramebufferSize at_125 = framebuffer_from_logical({1280.0, 720.0}, 1.25);
    CHECK(at_125 == FramebufferSize{1600, 900});

    // 151.5 rounds to 152 so that a 101 DIP wide window is never one pixel short of its scale.
    CHECK(framebuffer_from_logical({101.0, 50.0}, 1.5) == FramebufferSize{152, 75});

    const LogicalSize back = logical_from_framebuffer({1600, 900}, 1.25);
    CHECK(back.width == 1280.0);
    CHECK(back.height == 720.0);
}

TEST_CASE("Zero and negative sizes map to empty framebuffers", "[unit]") {
    CHECK(framebuffer_from_logical({0.0, -5.0}, 1.0).empty());
    CHECK(framebuffer_from_logical({100.0, 100.0}, 0.0).empty());

    const LogicalSize zero = logical_from_framebuffer({0, 0}, 1.5);
    CHECK(zero.width == 0.0);
    CHECK(zero.height == 0.0);

    // A non-positive scale is treated as 100 percent, so the conversion never divides by zero.
    CHECK(logical_from_framebuffer({640, 480}, 0.0).width == 640.0);
}

TEST_CASE("Oversized logical sizes saturate instead of wrapping", "[unit]") {
    const FramebufferSize huge = framebuffer_from_logical({1.0e12, 1.0}, 1.0);
    CHECK(huge.width == std::numeric_limits<std::uint32_t>::max());
    CHECK(huge.height == 1);
}

TEST_CASE("A default window description is valid", "[unit]") {
    CHECK(validate_window_desc(WindowDesc{}).ok());
}

TEST_CASE("Window titles must be valid UTF-8 without NUL bytes and may use Cyrillic", "[unit]") {
    WindowDesc desc;

    desc.title = "\xC3\x28";  // invalid UTF-8 continuation
    auto invalid = validate_window_desc(desc);
    REQUIRE_FALSE(invalid.ok());
    CHECK(invalid.error().code == PlatformErrorCode::InvalidArgument);

    desc.title = std::string("bad\0title", 9);
    CHECK_FALSE(validate_window_desc(desc).ok());

    desc.title = "Редактор мира";
    CHECK(validate_window_desc(desc).ok());

    desc.title = "";
    CHECK(validate_window_desc(desc).ok());
}

TEST_CASE("Window sizes must be finite, at least one DIP, and within the maximum", "[unit]") {
    const auto with_size = [](double width, double height) {
        WindowDesc desc;
        desc.size = {width, height};
        return validate_window_desc(desc);
    };

    CHECK(with_size(1.0, 1.0).ok());
    CHECK_FALSE(with_size(0.0, 720.0).ok());
    CHECK_FALSE(with_size(1280.0, -1.0).ok());
    CHECK_FALSE(with_size(std::numeric_limits<double>::quiet_NaN(), 720.0).ok());
    CHECK_FALSE(with_size(1280.0, std::numeric_limits<double>::infinity()).ok());
    CHECK_FALSE(with_size(engine::platform::kMaxLogicalDimension + 1.0, 720.0).ok());
}

TEST_CASE("Consecutive resizes of one window coalesce to the latest size", "[unit]") {
    EventQueue queue;
    const WindowId window{0, 1};
    queue.push(resized(window, 800, 600));
    queue.push(resized(window, 810, 600));
    queue.push(resized(window, 900, 700));

    const auto events = queue.drain();
    REQUIRE(events.size() == 1);
    const auto& data = std::get<WindowResized>(events[0].data);
    CHECK(data.framebuffer_size == FramebufferSize{900, 700});
}

TEST_CASE("Resizes of different windows are not merged", "[unit]") {
    EventQueue queue;
    queue.push(resized(WindowId{0, 1}, 800, 600));
    queue.push(resized(WindowId{1, 1}, 400, 300));
    queue.push(resized(WindowId{0, 1}, 820, 620));

    const auto events = queue.drain();
    CHECK(events.size() == 3);
}

TEST_CASE("Other events keep their order and are not coalesced", "[unit]") {
    EventQueue queue;
    const WindowId window{0, 1};
    queue.push(resized(window, 800, 600));
    queue.push(WindowEvent{window, WindowMinimized{}});
    queue.push(resized(window, 0, 0));
    queue.push(WindowEvent{window, WindowRestored{}});
    queue.push(WindowEvent{window, CloseRequested{}});

    const auto events = queue.drain();
    REQUIRE(events.size() == 5);
    CHECK(std::holds_alternative<WindowResized>(events[0].data));
    CHECK(std::holds_alternative<WindowMinimized>(events[1].data));
    CHECK(std::holds_alternative<WindowResized>(events[2].data));
    CHECK(std::holds_alternative<WindowRestored>(events[3].data));
    CHECK(std::holds_alternative<CloseRequested>(events[4].data));
}

TEST_CASE("Focus and DPI events carry their values", "[unit]") {
    EventQueue queue;
    const WindowId window{0, 1};
    queue.push(WindowEvent{window, WindowFocusChanged{true}});
    queue.push(WindowEvent{window, WindowDpiChanged{1.5}});

    const auto events = queue.drain();
    REQUIRE(events.size() == 2);
    CHECK(std::get<WindowFocusChanged>(events[0].data).focused);
    CHECK(std::get<WindowDpiChanged>(events[1].data).dpi_scale == 1.5);
}

TEST_CASE("drain returns events once and leaves the queue empty", "[unit]") {
    EventQueue queue;
    queue.push(WindowEvent{WindowId{0, 1}, CloseRequested{}});

    CHECK(queue.drain().size() == 1);
    CHECK(queue.empty());
    CHECK(queue.drain().empty());
}

TEST_CASE("Platform error codes have stable names", "[unit]") {
    CHECK(to_string(PlatformErrorCode::Unsupported) == "Unsupported");
    CHECK(to_string(PlatformErrorCode::InvalidArgument) == "InvalidArgument");
    CHECK(to_string(PlatformErrorCode::CreateFailed) == "CreateFailed");
    CHECK(to_string(PlatformErrorCode::NotFound) == "NotFound");
    CHECK(to_string(PlatformErrorCode::NoDisplay) == "NoDisplay");
}

// Windows (Win32) and Linux (X11) have backends. Any other operating system must fail explicitly instead of returning
// a fake window.
#if !defined(_WIN32) && !defined(__linux__)
TEST_CASE("Without an in-house backend, Platform::create reports Unsupported", "[unit]") {
    auto created = Platform::create();
    REQUIRE_FALSE(created.ok());
    CHECK(created.error().code == PlatformErrorCode::Unsupported);
}
#endif
