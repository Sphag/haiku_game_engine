// In-house Win32 windowing (US-007). Uses user32 directly; no SDL or GLFW.
//
// Message handlers only record state and queue events. They never run application code, except the modal tick,
// which runs inside the OS move/size loop. While any message or the tick is on the stack (dispatch_depth > 0), a
// window is not destroyed. destroy_window() marks it, and reap() destroys it when the outermost dispatch returns.
// This keeps a WndProc from running on a window that has already been freed.
#include "engine/platform/platform.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/log.hpp"
#include "engine/core/utf8.hpp"
#include "engine/platform/dpi.hpp"
#include "engine/platform/event_queue.hpp"
#include "engine/platform/input_mapping.hpp"
#include "engine/platform/input_state.hpp"
#include "engine/platform/slot_table.hpp"
#include "input_win32.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <climits>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace engine::platform {

namespace {

constexpr UINT_PTR kModalTickTimerId = 1;
constexpr UINT kModalTickIntervalMs = 16;

// HID usage page and usage for the mouse (HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_MOUSE from hidusage.h).
constexpr USHORT kHidUsagePageGenericDesktop = 0x01;
constexpr USHORT kHidUsageMouse = 0x02;

// Each Platform registers its own window class so two instances in one process do not share a WndProc context.
std::atomic<std::uint32_t> g_class_serial{0};

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

std::string last_error_text(DWORD code) {
    return "Win32 error " + std::to_string(code);
}

}  // namespace

struct Platform::Impl {
    // Stable per-window context stored in GWLP_USERDATA. It lives on the heap and is owned by its WindowRecord,
    // so it does not move when the slot table grows.
    struct Binding {
        Impl* impl = nullptr;
        WindowId id;
    };

    struct WindowRecord {
        std::unique_ptr<Binding> binding;
        HWND hwnd = nullptr;  // null once WM_NCDESTROY has run
        LogicalSize logical_size;
        FramebufferSize framebuffer_size;
        double dpi_scale = 1.0;
        bool focused = false;
        bool minimized = false;
        bool destroy_requested = false;
        bool capture_requested = false;  // cursor capture the application asked for; applied only while focused
    };

    HINSTANCE instance = nullptr;
    std::wstring class_name;
    bool class_registered = false;
    bool raw_mouse_registered = false;
    std::thread::id owner;

    SlotTable<WindowRecord> windows;
    EventQueue events;
    ModalTick modal_tick;
    int dispatch_depth = 0;
    std::exception_ptr pending_exception;  // thrown by application code inside a message; rethrown by pump_events

    // Input. live_input mirrors what has been queued, so a redundant transition is never queued. input_events holds the
    // queue that drain_input_events() returns.
    InputState live_input;
    std::vector<InputEvent> input_events;
    Utf16Decoder text_decoder;
    WindowId clip_owner;  // the window whose client area currently holds ClipCursor; invalid when none

    ~Impl() {
        // Destruction runs with no dispatch in progress, so every window is destroyed at once.
        std::vector<WindowId> ids;
        windows.for_each([&ids](WindowId id, WindowRecord&) { ids.push_back(id); });
        for (const WindowId id : ids) {
            destroy_now(id);
        }
        if (raw_mouse_registered) {
            // RIDEV_REMOVE needs hwndTarget == nullptr, which is how the device was registered.
            RAWINPUTDEVICE mouse{};
            mouse.usUsagePage = kHidUsagePageGenericDesktop;
            mouse.usUsage = kHidUsageMouse;
            mouse.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        }
        if (class_registered) {
            UnregisterClassW(class_name.c_str(), instance);
        }
    }

    // Adds an input event to the queue unless it repeats the current state.
    void queue_input(InputEvent event) {
        if (live_input.apply(event)) {
            input_events.push_back(std::move(event));
        }
    }

    [[nodiscard]] bool any_mouse_button_down() const {
        for (std::size_t i = 0; i < static_cast<std::size_t>(MouseButton::Count); ++i) {
            if (live_input.mouse_down(static_cast<MouseButton>(i))) {
                return true;
            }
        }
        return false;
    }

    // Releases every key and mouse button of `id`. Called when the window loses focus, so no key stays down after
    // the window goes inactive and the key-up message is never delivered.
    void release_held_input(WindowId id) {
        for (InputEvent& release : live_input.release_held(id)) {
            queue_input(std::move(release));
        }
        text_decoder.reset();
    }

    // Applies the cursor clip for `id` when it should hold one: focused, capture requested, not minimized. Releases
    // the clip when `id` holds it and should not.
    void update_clip(WindowId id) {
        WindowRecord* record = windows.find(id);
        const bool should_clip = record != nullptr && record->hwnd != nullptr && record->focused &&
                                 record->capture_requested && !record->minimized;
        if (should_clip) {
            RECT client{};
            GetClientRect(record->hwnd, &client);
            POINT top_left{client.left, client.top};
            POINT bottom_right{client.right, client.bottom};
            ClientToScreen(record->hwnd, &top_left);
            ClientToScreen(record->hwnd, &bottom_right);
            const RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
            ClipCursor(&screen);
            clip_owner = id;
        } else if (clip_owner == id) {
            ClipCursor(nullptr);
            clip_owner = WindowId{};
        }
    }

    // Reads XInput for every slot and queues the changes: connections, buttons, and axes. An empty slot diffs to
    // releases and a disconnect, so a pad that is unplugged mid-press does not leave a held button behind.
    void poll_gamepads() {
        std::vector<InputEvent> changes;
        for (std::uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
            diff_gamepad(slot, live_input, win32::read_xinput_pad(slot), changes);
        }
        for (InputEvent& change : changes) {
            queue_input(std::move(change));
        }
    }

    void require_owner() const {
        ENGINE_ASSERT(std::this_thread::get_id() == owner,
                      "Platform calls must come from the thread that created the Platform");
    }

    [[nodiscard]] core::Result<WindowId, PlatformError> create_window(const WindowDesc& desc) {
        require_owner();
        if (auto valid = validate_window_desc(desc); !valid.ok()) {
            return core::failure(valid.error());
        }
        if (desc.title.size() > static_cast<std::size_t>(INT_MAX)) {
            return core::failure(PlatformError{PlatformErrorCode::InvalidArgument, "window title is too long"});
        }

        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!desc.resizable) {
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }

        // The initial rectangle is sized for the system DPI. Once the window exists, its real DPI decides the final
        // client size, because the window may have opened on a monitor with a different scale.
        const UINT system_dpi = GetDpiForSystem();
        const double system_scale = dpi_scale_from_dpi(system_dpi);
        const FramebufferSize initial = framebuffer_from_logical(desc.size, system_scale);
        RECT rect{0, 0, static_cast<LONG>(initial.width), static_cast<LONG>(initial.height)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, system_dpi);

        WindowRecord fresh;
        fresh.binding = std::make_unique<Binding>();
        fresh.binding->impl = this;
        fresh.dpi_scale = system_scale;
        Binding* binding = fresh.binding.get();
        const WindowId id = windows.insert(std::move(fresh));
        binding->id = id;

        const std::wstring title = utf8_to_wide(desc.title);
        // WM_NCCREATE receives the binding through lpParam and stores it in GWLP_USERDATA.
        HWND hwnd = CreateWindowExW(0, class_name.c_str(), title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                    rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                                    binding);
        if (hwnd == nullptr) {
            const DWORD error = GetLastError();
            windows.erase(id);
            return core::failure(PlatformError{PlatformErrorCode::CreateFailed,
                                               "CreateWindowExW failed: " + last_error_text(error)});
        }

        WindowRecord* record = windows.find(id);
        const UINT actual_dpi = GetDpiForWindow(hwnd);
        const double actual_scale = dpi_scale_from_dpi(actual_dpi);
        if (actual_scale != record->dpi_scale) {
            record->dpi_scale = actual_scale;
            events.push(WindowEvent{id, WindowDpiChanged{actual_scale}});
        }
        set_client_size(hwnd, framebuffer_from_logical(desc.size, actual_scale), actual_dpi);
        // SetWindowPos sends WM_SIZE only when the size changes, so read the client size explicitly. The application
        // then always receives the initial size as an event.
        RECT client{};
        GetClientRect(hwnd, &client);
        record->framebuffer_size = {static_cast<std::uint32_t>(client.right - client.left),
                                    static_cast<std::uint32_t>(client.bottom - client.top)};
        record->logical_size = logical_from_framebuffer(record->framebuffer_size, record->dpi_scale);
        events.push(WindowEvent{id, WindowResized{record->logical_size, record->framebuffer_size}});
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return core::success(id);
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> destroy_window(WindowId id) {
        require_owner();
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        if (dispatch_depth > 0) {
            record->destroy_requested = true;
            return core::success();
        }
        destroy_now(id);
        return core::success();
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> show_state(WindowId id, int command) {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr || record->hwnd == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        ShowWindow(record->hwnd, command);
        return core::success();
    }

    [[nodiscard]] core::Result<WindowInfo, PlatformError> window_info(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        return core::success(WindowInfo{id, record->logical_size, record->framebuffer_size, record->dpi_scale,
                                        record->focused, record->minimized});
    }

    [[nodiscard]] core::Result<Win32WindowHandles, PlatformError> win32_handles(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr || record->hwnd == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        return core::success(Win32WindowHandles{instance, record->hwnd});
    }

    void pump_events() {
        require_owner();
        ENGINE_ASSERT(dispatch_depth == 0, "pump_events must not be called from a window message or the modal tick");

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (msg.message == WM_QUIT) {
                // Nothing in the engine posts WM_QUIT. Re-post it so it is not lost if something else does.
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            ++dispatch_depth;
            // Without TranslateMessage a key press never becomes WM_CHAR, so no text could be typed (text fields, chat, console).
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            --dispatch_depth;
        }
        // Gamepads are polled, not messaged. Polling after the messages keeps a press and its release in the order the
        // controller reported them.
        poll_gamepads();
        reap();
        rethrow_pending();
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> set_cursor_capture(WindowId id, bool captured) {
        require_owner();
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        record->capture_requested = captured;
        update_clip(id);
        return core::success();
    }

    [[nodiscard]] core::Result<bool, PlatformError> cursor_captured(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        return core::success(record->capture_requested);
    }

    [[nodiscard]] core::Result<PointerPosition, PlatformError> pointer_position(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr || record->hwnd == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        POINT screen{};
        if (!GetCursorPos(&screen)) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "the cursor position is not available"});
        }
        // Screen coordinates map to the client area in device pixels, the same units as the framebuffer.
        ScreenToClient(record->hwnd, &screen);
        return core::success(PointerPosition{static_cast<double>(screen.x), static_cast<double>(screen.y)});
    }

    void set_modal_tick(ModalTick tick) {
        require_owner();
        modal_tick = std::move(tick);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* binding = reinterpret_cast<Binding*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (binding == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        try {
            return binding->impl->on_message(binding->id, hwnd, message, wparam, lparam);
        } catch (...) {
            // C++ exceptions must not unwind through the OS callback. Keep the first one; pump_events rethrows it.
            if (!binding->impl->pending_exception) {
                binding->impl->pending_exception = std::current_exception();
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    LRESULT on_message(WindowId id, HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        switch (message) {
            case WM_NCCREATE:
                record->hwnd = hwnd;
                break;

            case WM_NCDESTROY:
                record->hwnd = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;

            case WM_CLOSE:
                // Leave the window open. The application decides when to call destroy_window().
                events.push(WindowEvent{id, CloseRequested{}});
                return 0;

            case WM_SIZE: {
                // lParam carries the client size in physical pixels (per-monitor DPI aware process).
                if (wparam == SIZE_MINIMIZED) {
                    if (!record->minimized) {
                        record->minimized = true;
                        events.push(WindowEvent{id, WindowMinimized{}});
                    }
                    record->framebuffer_size = {};
                    record->logical_size = {};
                } else {
                    if (record->minimized) {
                        record->minimized = false;
                        events.push(WindowEvent{id, WindowRestored{}});
                    }
                    record->framebuffer_size = {static_cast<std::uint32_t>(LOWORD(lparam)),
                                                static_cast<std::uint32_t>(HIWORD(lparam))};
                    record->logical_size = logical_from_framebuffer(record->framebuffer_size, record->dpi_scale);
                }
                events.push(WindowEvent{id, WindowResized{record->logical_size, record->framebuffer_size}});
                update_clip(id);  // a minimized window holds no clip; a restored one gets its clip back
                return 0;
            }

            case WM_MOVE:
                update_clip(id);  // ClipCursor uses screen coordinates, so a moved window needs a new clip
                break;

            case WM_DPICHANGED: {
                // The scale must change before SetWindowPos, because that sends WM_SIZE with the new client size.
                // The suggested rectangle keeps the logical size the same on the new monitor.
                record->dpi_scale = dpi_scale_from_dpi(static_cast<std::uint32_t>(LOWORD(wparam)));
                events.push(WindowEvent{id, WindowDpiChanged{record->dpi_scale}});
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }

            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                record->focused = message == WM_SETFOCUS;
                if (!record->focused) {
                    release_held_input(id);  // queued before the focus event, so the order matches what happened
                }
                update_clip(id);
                events.push(WindowEvent{id, WindowFocusChanged{record->focused}});
                return 0;

            case WM_SETCURSOR:
                // A captured cursor is hidden while the window is focused and the pointer is over the client area.
                if (record->focused && record->capture_requested && LOWORD(lparam) == HTCLIENT) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;

            case WM_INPUT: {
                // Raw Input reports are read here and then handed to DefWindowProc, which the API requires for cleanup.
                if (GET_RAWINPUT_CODE_WPARAM(wparam) != RIM_INPUT) {
                    break;
                }
                UINT size = 0;
                GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
                if (size == 0) {
                    break;
                }
                std::vector<unsigned char> buffer(size);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &size,
                                    sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
                    break;
                }
                const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    // Only relative motion comes from here. Raw Input follows keyboard focus instead of the window under the
                    // cursor, so a click on a window that overlaps this one, or the click that activates this one, would be
                    // reported wrongly. Buttons and the wheel come from the window messages below.
                    for (InputEvent& event : win32::raw_mouse_events(raw->data.mouse, id)) {
                        if (std::holds_alternative<MouseMoved>(event.data)) {
                            queue_input(std::move(event));
                        }
                    }
                }
                break;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP: {
                MouseButton button = MouseButton::Left;
                bool down = false;
                switch (message) {
                    case WM_LBUTTONDOWN: button = MouseButton::Left; down = true; break;
                    case WM_LBUTTONUP: button = MouseButton::Left; break;
                    case WM_RBUTTONDOWN: button = MouseButton::Right; down = true; break;
                    case WM_RBUTTONUP: button = MouseButton::Right; break;
                    case WM_MBUTTONDOWN: button = MouseButton::Middle; down = true; break;
                    case WM_MBUTTONUP: button = MouseButton::Middle; break;
                    default:
                        button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2;
                        down = message == WM_XBUTTONDOWN;
                        break;
                }
                if (down) {
                    // The window keeps the mouse while a button is held, so a drag that leaves the client area still ends
                    // with its release here instead of being lost.
                    SetCapture(hwnd);
                    queue_input(InputEvent{id, MouseButtonPressed{button}});
                } else {
                    queue_input(InputEvent{id, MouseButtonReleased{button}});
                    if (!any_mouse_button_down()) {
                        ReleaseCapture();
                    }
                }
                // The X button messages ask for TRUE to say they were handled.
                return (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) ? TRUE : 0;
            }

            case WM_CAPTURECHANGED:
                // Capture taken away while a button is held (a system dialog, Alt+Tab): the release will never arrive.
                for (InputEvent& release : live_input.release_held(id)) {
                    if (std::holds_alternative<MouseButtonReleased>(release.data)) {
                        queue_input(std::move(release));
                    }
                }
                return 0;

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                const double notches = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                if (message == WM_MOUSEWHEEL) {
                    queue_input(InputEvent{id, MouseWheelScrolled{0.0, notches}});
                } else {
                    queue_input(InputEvent{id, MouseWheelScrolled{notches, 0.0}});
                }
                return 0;
            }

            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP: {
                // Auto-repeat arrives as more WM_KEYDOWN messages. queue_input drops them because the key is already
                // held, so no repeat reaches the queue.
                const Key key = win32::key_from_message(wparam, lparam);
                if (key != Key::Unknown) {
                    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                    if (down) {
                        queue_input(InputEvent{id, KeyPressed{key}});
                    } else {
                        queue_input(InputEvent{id, KeyReleased{key}});
                    }
                }
                // System keys still reach DefWindowProc: Alt+F4 closes the window and F10 opens the menu bar.
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) {
                    break;
                }
                return 0;
            }

            case WM_CHAR:
                // WM_CHAR delivers UTF-16 units. A surrogate pair becomes one code point; control keys are dropped.
                if (const auto codepoint = text_decoder.push(static_cast<char16_t>(wparam))) {
                    queue_input(InputEvent{id, TextInput{encode_utf8(*codepoint)}});
                }
                return 0;

            case WM_ENTERSIZEMOVE:
                // The OS keeps the thread in its own loop during a drag. A timer lets the modal tick keep rendering.
                SetTimer(hwnd, kModalTickTimerId, kModalTickIntervalMs, nullptr);
                return 0;

            case WM_EXITSIZEMOVE:
                KillTimer(hwnd, kModalTickTimerId);
                return 0;

            case WM_TIMER:
                if (wparam == kModalTickTimerId) {
                    run_modal_tick();
                    return 0;
                }
                break;

            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void run_modal_tick() {
        if (!modal_tick) {
            return;
        }
        // Copy the callable: the tick may replace it through set_modal_tick().
        const ModalTick tick = modal_tick;
        ++dispatch_depth;
        try {
            tick();
        } catch (...) {
            if (!pending_exception) {
                pending_exception = std::current_exception();
            }
        }
        --dispatch_depth;
    }

    void destroy_now(WindowId id) {
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return;
        }
        if (record->hwnd != nullptr) {
            // WM_NCDESTROY runs inside DestroyWindow and clears record->hwnd and the binding link.
            DestroyWindow(record->hwnd);
        }
        windows.erase(id);
        update_clip(id);  // the record is gone, so this releases a clip the window was holding
    }

    // Destroys the windows whose destruction was requested during dispatch. Runs only when no dispatch is active.
    void reap() {
        if (dispatch_depth != 0) {
            return;
        }
        std::vector<WindowId> ids;
        windows.for_each([&ids](WindowId id, WindowRecord& record) {
            if (record.destroy_requested) {
                ids.push_back(id);
            }
        });
        for (const WindowId id : ids) {
            destroy_now(id);
        }
    }

    void rethrow_pending() {
        if (pending_exception) {
            std::exception_ptr error = std::exchange(pending_exception, nullptr);
            std::rethrow_exception(error);
        }
    }

    static void set_client_size(HWND hwnd, FramebufferSize size, UINT dpi) {
        RECT rect{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
};

core::Result<std::unique_ptr<Platform>, PlatformError> Platform::create() {
    // Per-monitor DPI awareness v2 must be set before any window exists. If the host process already chose a
    // different mode, keep it and report that in the log.
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE &&
        GetLastError() != ERROR_ACCESS_DENIED) {
        core::log_message(core::LogLevel::Warning, "platform", "could not enable per-monitor DPI awareness");
    }

    auto impl = std::make_unique<Impl>();
    impl->instance = GetModuleHandleW(nullptr);
    impl->owner = std::this_thread::get_id();
    impl->class_name = L"EngineWindow" + std::to_wstring(g_class_serial.fetch_add(1) + 1);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &Impl::window_proc;
    window_class.hInstance = impl->instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.lpszClassName = impl->class_name.c_str();
    if (RegisterClassExW(&window_class) == 0) {
        return core::failure(PlatformError{PlatformErrorCode::CreateFailed,
                                           "RegisterClassExW failed: " + last_error_text(GetLastError())});
    }
    impl->class_registered = true;

    // Raw Input carries the mouse: relative motion, buttons, and wheel. The keyboard and text come from window messages
    // only, because a raw keyboard registration would report every key a second time. hwndTarget stays null, so the
    // device follows keyboard focus and only the focused window receives its reports.
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = kHidUsagePageGenericDesktop;
    mouse.usUsage = kHidUsageMouse;
    mouse.dwFlags = 0;
    mouse.hwndTarget = nullptr;
    if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) == FALSE) {
        return core::failure(PlatformError{PlatformErrorCode::CreateFailed,
                                           "RegisterRawInputDevices failed: " + last_error_text(GetLastError())});
    }
    impl->raw_mouse_registered = true;

    return core::success(std::unique_ptr<Platform>(new Platform(std::move(impl))));
}

Platform::Platform(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Platform::~Platform() = default;

core::Result<WindowId, PlatformError> Platform::create_window(const WindowDesc& desc) {
    return impl_->create_window(desc);
}

core::Result<core::Unit, PlatformError> Platform::destroy_window(WindowId id) {
    return impl_->destroy_window(id);
}

core::Result<core::Unit, PlatformError> Platform::minimize_window(WindowId id) {
    return impl_->show_state(id, SW_MINIMIZE);
}

core::Result<core::Unit, PlatformError> Platform::restore_window(WindowId id) {
    return impl_->show_state(id, SW_RESTORE);
}

core::Result<WindowInfo, PlatformError> Platform::window_info(WindowId id) const {
    return impl_->window_info(id);
}

core::Result<Win32WindowHandles, PlatformError> Platform::win32_handles(WindowId id) const {
    return impl_->win32_handles(id);
}

core::Result<XlibWindowHandles, PlatformError> Platform::xlib_handles(WindowId) const {
    return core::failure(PlatformError{PlatformErrorCode::Unsupported,
                                       "Xlib handles exist only on the X11 backend; use win32_handles"});
}

void Platform::pump_events() {
    impl_->pump_events();
}

std::vector<WindowEvent> Platform::drain_events() {
    impl_->require_owner();
    return impl_->events.drain();
}

void Platform::set_modal_tick(ModalTick tick) {
    impl_->set_modal_tick(std::move(tick));
}

std::size_t Platform::window_count() const noexcept {
    return impl_->windows.size();
}

std::vector<InputEvent> Platform::drain_input_events() {
    impl_->require_owner();
    return std::exchange(impl_->input_events, std::vector<InputEvent>{});
}

core::Result<core::Unit, PlatformError> Platform::set_cursor_capture(WindowId id, bool captured) {
    return impl_->set_cursor_capture(id, captured);
}

core::Result<bool, PlatformError> Platform::cursor_captured(WindowId id) const {
    return impl_->cursor_captured(id);
}

core::Result<PointerPosition, PlatformError> Platform::pointer_position(WindowId id) const {
    return impl_->pointer_position(id);
}

}  // namespace engine::platform
