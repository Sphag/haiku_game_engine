// In-house X11 windowing and input (US-008, US-010). Talks to the X server through the Xlib subset in xlib_abi.hpp; no
// SDL or GLFW.
//
// X11 has no modal size loop: the window manager resizes a window without holding the application's thread, so the
// frame loop keeps running during a drag and the modal tick is never invoked on this backend. Application code never
// runs inside an X event, so destroy_window() takes effect at once.
//
// Input follows the Win32 contract. Keys and text go to the focused window, relative mouse motion comes from XInput2
// raw events (delivered to the root window, so they are routed to the focused window here), and gamepads come from
// evdev (gamepads_linux.hpp). Focus loss releases every held key and button. Cursor capture grabs the pointer, confines
// it to the window, and hides it with an empty cursor.
#include "engine/platform/platform.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/log.hpp"
#include "engine/platform/dpi.hpp"
#include "engine/platform/event_queue.hpp"
#include "engine/platform/input_mapping.hpp"
#include "engine/platform/input_state.hpp"
#include "engine/platform/slot_table.hpp"
#include "gamepads_linux.hpp"
#include "xlib_abi.hpp"

#include <atomic>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::platform {

namespace {

// Xlib reports protocol errors through a process-wide handler. The default handler exits the process, so ours only
// records the error. create_window compares the count before and after its requests to detect a rejected window.
std::atomic<unsigned> g_x_error_count{0};
const x11::Api* g_api = nullptr;

// Modifier bits in XKeyEvent::state that suppress text, as Win32 does for Ctrl, Alt, and Win chords.
constexpr unsigned int kControlMask = 1U << 2;
constexpr unsigned int kMod1Mask = 1U << 3;  // Alt
constexpr unsigned int kMod4Mask = 1U << 6;  // Super
constexpr unsigned int kShiftMask = 1U << 0;

// Buffer for one Xutf8LookupString call. A composed character is far shorter than this.
constexpr int kTextBufferBytes = 256;

int handle_x_error(x11::Display* display, x11::XErrorEvent* event) {
    g_x_error_count.fetch_add(1);
    char text[256] = {};
    if (g_api != nullptr) {
        g_api->XGetErrorText(display, event->error_code, text, static_cast<int>(sizeof(text)));
    }
    core::log_message(core::LogLevel::Warning, "platform", std::string("X11 protocol error: ") + text);
    return 0;
}

}  // namespace

struct Platform::Impl {
    struct WindowRecord {
        x11::Window window = 0;
        LogicalSize logical_size;
        FramebufferSize framebuffer_size;
        double dpi_scale = 1.0;
        bool focused = false;
        bool minimized = false;
        bool captured = false;     // the application asked for capture; it is applied only while focused
        x11::XIC input_context = nullptr;  // null when no input method serves this window
    };

    const x11::Api* api = nullptr;
    x11::Display* display = nullptr;
    int screen = 0;
    x11::Window root = 0;
    x11::Atom wm_protocols = 0;
    x11::Atom wm_delete_window = 0;
    x11::Atom net_wm_name = 0;
    x11::Atom utf8_string = 0;
    double dpi_scale = 1.0;
    x11::XErrorHandler previous_error_handler = nullptr;
    std::thread::id owner;

    // Input. live_input mirrors what has been queued, so a redundant transition is never queued (as on Win32).
    const x11::XiApi* xi = nullptr;  // null when libXi is missing; relative mouse motion is then unavailable
    int xi_opcode = 0;               // XInputExtension's major opcode, to recognise its generic events
    x11::XIM input_method = nullptr;
    x11::Cursor hidden_cursor = 0;   // created on first capture
    std::optional<WindowId> focused_window;
    std::optional<WindowId> grabbing_window;
    // Captured motion is measured from `capture_origin`, the last reported pointer position. The pointer starts at the
    // centre (warped before the grab) and is warped back whenever it nears an edge.
    std::int32_t capture_origin_x = 0;
    std::int32_t capture_origin_y = 0;
    InputState live_input;
    std::vector<InputEvent> input_events;
    linux_input::GamepadReader gamepads;

    SlotTable<WindowRecord> windows;
    std::unordered_map<x11::Window, WindowId> ids_by_window;
    EventQueue events;
    ModalTick modal_tick;  // stored for API parity; never invoked on X11 (see the file comment)

    ~Impl() {
        std::vector<WindowId> ids;
        windows.for_each([&ids](WindowId id, WindowRecord&) { ids.push_back(id); });
        for (const WindowId id : ids) {
            destroy_now(id);
        }
        if (display != nullptr) {
            if (hidden_cursor != 0) {
                api->XFreeCursor(display, hidden_cursor);
            }
            if (input_method != nullptr) {
                api->XCloseIM(input_method);
            }
            api->XSetErrorHandler(previous_error_handler);
            api->XCloseDisplay(display);
            g_api = nullptr;
        }
    }

    // Adds an input event to the queue unless it repeats the current state.
    void queue_input(InputEvent event) {
        if (live_input.apply(event)) {
            input_events.push_back(std::move(event));
        }
    }

    // Releases every key and button the window holds. Called on focus loss, so no key stays stuck.
    void release_held_input(WindowId id) {
        for (InputEvent& release : live_input.release_held(id)) {
            queue_input(std::move(release));
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

        const FramebufferSize initial = framebuffer_from_logical(desc.size, dpi_scale);
        const unsigned errors_before = g_x_error_count.load();
        const x11::Window window =
            api->XCreateWindow(display, root, 0, 0, initial.width, initial.height, 0, x11::kCopyFromParent,
                               x11::kInputOutput, nullptr, 0, nullptr);
        api->XSelectInput(display, window,
                          x11::kStructureNotifyMask | x11::kFocusChangeMask | x11::kKeyPressMask |
                              x11::kKeyReleaseMask | x11::kButtonPressMask | x11::kButtonReleaseMask);

        // _NET_WM_NAME carries UTF-8, so Cyrillic titles reach the window manager unchanged.
        const auto* title = reinterpret_cast<const unsigned char*>(desc.title.data());
        api->XChangeProperty(display, window, net_wm_name, utf8_string, 8, x11::kPropModeReplace, title,
                             static_cast<int>(desc.title.size()));

        x11::Atom protocols[] = {wm_delete_window};
        api->XSetWMProtocols(display, window, protocols, 1);

        if (!desc.resizable) {
            x11::XSizeHints hints{};
            hints.flags = x11::kPMinSize | x11::kPMaxSize;
            hints.min_width = hints.max_width = static_cast<int>(initial.width);
            hints.min_height = hints.max_height = static_cast<int>(initial.height);
            api->XSetWMNormalHints(display, window, &hints);
        }

        api->XMapRaised(display, window);
        api->XSync(display, 0);
        if (g_x_error_count.load() != errors_before) {
            api->XDestroyWindow(display, window);
            api->XSync(display, 0);
            return core::failure(PlatformError{PlatformErrorCode::CreateFailed,
                                               "the X server rejected the window; see the log for the protocol error"});
        }

        // Read the size the server actually granted. A window manager may not honour the requested size.
        x11::Window geometry_root = 0;
        int x = 0;
        int y = 0;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int border = 0;
        unsigned int depth = 0;
        api->XGetGeometry(display, window, &geometry_root, &x, &y, &width, &height, &border, &depth);

        WindowRecord fresh;
        fresh.window = window;
        fresh.dpi_scale = dpi_scale;
        if (input_method != nullptr) {
            // The input context needs the window; the preedit and status are drawn by nothing (the app takes committed
            // text only), so the input method does not add a pop-up over the game.
            fresh.input_context = api->XCreateIC(input_method, x11::kNInputStyle,
                                                 x11::kIMPreeditNothing | x11::kIMStatusNothing,
                                                 x11::kNClientWindow, window, x11::kNFocusWindow, window,
                                                 static_cast<char*>(nullptr));
        }
        fresh.framebuffer_size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        fresh.logical_size = logical_from_framebuffer(fresh.framebuffer_size, dpi_scale);
        const WindowId id = windows.insert(fresh);
        ids_by_window[window] = id;
        events.push(WindowEvent{id, WindowResized{fresh.logical_size, fresh.framebuffer_size}});
        return core::success(id);
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> destroy_window(WindowId id) {
        require_owner();
        if (windows.find(id) == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        destroy_now(id);
        return core::success();
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> set_cursor_capture(WindowId id, bool captured) {
        require_owner();
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        record->captured = captured;
        if (captured && record->focused && !record->minimized) {
            apply_grab(id);
        } else if (!captured) {
            release_grab(id);
        }
        return core::success();
    }

    [[nodiscard]] core::Result<bool, PlatformError> cursor_captured(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        return core::success(record->captured);
    }

    [[nodiscard]] core::Result<PointerPosition, PlatformError> pointer_position(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        x11::Window root_window = x11::kNone;
        x11::Window child_window = x11::kNone;
        int root_x = 0;
        int root_y = 0;
        int window_x = 0;
        int window_y = 0;
        unsigned int button_mask = 0;
        // Zero means the pointer is on another screen, so there is no position relative to this window.
        if (api->XQueryPointer(display, record->window, &root_window, &child_window, &root_x, &root_y, &window_x, &window_y,
                               &button_mask) == 0) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "the pointer is on another screen"});
        }
        return core::success(PointerPosition{static_cast<double>(window_x), static_cast<double>(window_y)});
    }

    // Grabs the pointer for the window, confines it there, and hides it. The grab stops the pointer leaving the window;
    // relative motion keeps arriving from XInput2 whether or not the pointer moves.
    void apply_grab(WindowId id) {
        const WindowRecord* record = windows.find(id);
        if (record == nullptr || (grabbing_window && *grabbing_window == id)) {
            return;
        }
        if (grabbing_window) {
            release_grab(*grabbing_window);
        }
        if (hidden_cursor == 0) {
            hidden_cursor = make_hidden_cursor();
        }
        // Warp before the grab. A pointer outside the window would otherwise be confined to its edge by the grab, and that
        // jump would arrive as motion.
        warp_to_centre(*record);
        api->XSync(display, 0);
        const int status = api->XGrabPointer(display, record->window, 1,
                                             static_cast<unsigned int>(x11::kButtonPressMask | x11::kButtonReleaseMask |
                                                                       x11::kPointerMotionMask),
                                             x11::kGrabModeAsync, x11::kGrabModeAsync, record->window, hidden_cursor,
                                             x11::kCurrentTime);
        if (status != x11::kGrabSuccess) {
            core::log_message(core::LogLevel::Warning, "platform",
                              "cursor capture failed: another client holds the pointer (X grab status " +
                                  std::to_string(status) + ")");
            return;
        }
        grabbing_window = id;
        capture_origin_x = static_cast<std::int32_t>(record->framebuffer_size.width / 2);
        capture_origin_y = static_cast<std::int32_t>(record->framebuffer_size.height / 2);
        api->XFlush(display);
    }

    // Puts the pointer at the centre of the window, the origin for the next captured delta.
    void warp_to_centre(const WindowRecord& record) {
        if (record.framebuffer_size.width == 0 || record.framebuffer_size.height == 0) {
            return;
        }
        api->XWarpPointer(display, x11::kNone, record.window, 0, 0, 0, 0,
                          static_cast<int>(record.framebuffer_size.width / 2),
                          static_cast<int>(record.framebuffer_size.height / 2));
    }

    // While captured, relative motion comes from pointer motion: each event's offset from the last reported position.
    // Raw motion did not arrive while the pointer was grabbed on XWayland, so raw motion is used only when the pointer is
    // free. Near an edge the pointer is warped back to the centre, so the grab never confines it there.
    void handle_pointer_motion(const x11::XMotionEvent& motion) {
        if (!grabbing_window) {
            return;
        }
        const WindowRecord* record = windows.find(*grabbing_window);
        if (record == nullptr || record->framebuffer_size.width == 0 || record->framebuffer_size.height == 0) {
            return;
        }
        const auto width = static_cast<std::int32_t>(record->framebuffer_size.width);
        const auto height = static_cast<std::int32_t>(record->framebuffer_size.height);

        const std::int32_t dx = motion.x - capture_origin_x;
        const std::int32_t dy = motion.y - capture_origin_y;
        const bool reposition = dx > width || dx < -width || dy > height || dy < -height;
        capture_origin_x = motion.x;
        capture_origin_y = motion.y;
        // A step larger than the window is the server moving the pointer, not the user, so it is not reported as motion.
        if (!reposition && (dx != 0 || dy != 0)) {
            queue_input(InputEvent{*grabbing_window, MouseMoved{dx, dy}});
        }

        const std::int32_t margin_x = width / 8;
        const std::int32_t margin_y = height / 8;
        if (reposition || motion.x < margin_x || motion.x >= width - margin_x || motion.y < margin_y ||
            motion.y >= height - margin_y) {
            warp_to_centre(*record);
            capture_origin_x = width / 2;
            capture_origin_y = height / 2;
        }
    }

    void release_grab(WindowId id) {
        // GCC 9 does not derive != from ==, so the comparison is written as !(==).
        if (!grabbing_window || !(*grabbing_window == id)) {
            return;
        }
        api->XUngrabPointer(display, x11::kCurrentTime);
        api->XFlush(display);
        grabbing_window.reset();
    }

    // A 8x8 bitmap of zeros makes a cursor with no visible pixels. The bitmap is freed once the cursor exists.
    x11::Cursor make_hidden_cursor() {
        static const char kBlank[8] = {};
        const x11::Pixmap bitmap = api->XCreateBitmapFromData(display, root, kBlank, 8, 8);
        x11::XColor black{};
        const x11::Cursor cursor = api->XCreatePixmapCursor(display, bitmap, bitmap, &black, &black, 0, 0);
        api->XFreePixmap(display, bitmap);
        return cursor;
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> minimize_window(WindowId id) {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        // The window manager performs the iconify. WindowMinimized arrives once it unmaps the window.
        static_cast<void>(api->XIconifyWindow(display, record->window, screen));
        api->XFlush(display);
        return core::success();
    }

    [[nodiscard]] core::Result<core::Unit, PlatformError> restore_window(WindowId id) {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        // Mapping a minimized window asks the window manager to restore it.
        api->XMapRaised(display, record->window);
        api->XFlush(display);
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

    [[nodiscard]] core::Result<XlibWindowHandles, PlatformError> xlib_handles(WindowId id) const {
        require_owner();
        const WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return core::failure(PlatformError{PlatformErrorCode::NotFound, "window id is stale or unknown"});
        }
        return core::success(XlibWindowHandles{display, record->window, screen});
    }

    void pump_events() {
        require_owner();
        std::vector<x11::XEvent> batch;
        while (api->XPending(display) > 0) {
            x11::XEvent event{};
            api->XNextEvent(display, &event);
            batch.push_back(event);
        }
        for (std::size_t i = 0; i < batch.size(); ++i) {
            // An input method may take the event (composition keys); it then produces its text as a commit.
            if (api->XFilterEvent(&batch[i], x11::kNone)) {
                continue;
            }
            // Servers without detectable auto-repeat send release and press at the same time for a repeat. Such a pair is
            // a repeat, so both are dropped and the key stays held.
            if (batch[i].type == x11::kKeyRelease && i + 1 < batch.size() && batch[i + 1].type == x11::kKeyPress &&
                batch[i + 1].xkey.keycode == batch[i].xkey.keycode && batch[i + 1].xkey.time == batch[i].xkey.time) {
                ++i;
                continue;
            }
            handle_event(batch[i]);
        }
        poll_gamepads();
    }

    // Reads evdev for gamepads and queues the changes. An empty slot diffs to a disconnected snapshot, so an unplugged
    // pad releases its buttons and reports GamepadDisconnected.
    void poll_gamepads() {
        gamepads.poll();
        std::vector<InputEvent> changes;
        for (std::uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
            diff_gamepad(slot, live_input, gamepads.snapshot(slot), changes);
        }
        for (InputEvent& change : changes) {
            queue_input(std::move(change));
        }
    }

    // The keysym of a key's position (group 0), so keys are the same on every layout that types Latin letters at that
    // position. Keys with no Latin keysym in any group, such as a Cyrillic layout's letters, fall back to the group that has one.
    [[nodiscard]] Key key_for_keycode(x11::KeyCode keycode) const {
        for (int index = 0; index < 4; ++index) {
            const Key key = key_from_x11_keysym(static_cast<std::uint32_t>(api->XKeycodeToKeysym(display, keycode, index)));
            if (key != Key::Unknown) {
                return key;
            }
        }
        return Key::Unknown;
    }

    // Text for one key press, as UTF-8. Chords with Ctrl, Alt, or Super type nothing, as WM_CHAR does on Win32. The input
    // method is used when the window has one; otherwise the keysym types its character (no composition).
    [[nodiscard]] std::string text_for_key(const WindowRecord& record, x11::XKeyEvent& key) const {
        if ((key.state & (kControlMask | kMod1Mask | kMod4Mask)) != 0) {
            return {};
        }
        if (record.input_context != nullptr) {
            char buffer[kTextBufferBytes] = {};
            x11::KeySym keysym = 0;
            x11::Status status = 0;
            const int bytes = api->Xutf8LookupString(record.input_context, &key, buffer, kTextBufferBytes, &keysym,
                                                     &status);
            if (bytes <= 0) {
                return {};
            }
            return drop_control_characters(std::string(buffer, static_cast<std::size_t>(bytes)));
        }
        const int shift_level = (key.state & kShiftMask) != 0 ? 1 : 0;
        const auto keysym = static_cast<std::uint32_t>(api->XKeycodeToKeysym(display, key.keycode, shift_level));
        const auto codepoint = codepoint_from_keysym(keysym);
        if (!codepoint || !is_text_codepoint(*codepoint)) {
            return {};
        }
        return encode_utf8(*codepoint);
    }

    void handle_key(x11::XKeyEvent& key, bool pressed) {
        WindowId id;
        if (!lookup(key.window, id)) {
            return;
        }
        const Key mapped = key_for_keycode(key.keycode);
        if (pressed) {
            if (mapped != Key::Unknown) {
                queue_input(InputEvent{id, KeyPressed{mapped}});
            }
            const WindowRecord* record = windows.find(id);
            if (record != nullptr) {
                std::string text = text_for_key(*record, key);
                if (!text.empty()) {
                    queue_input(InputEvent{id, TextInput{std::move(text)}});
                }
            }
        } else if (mapped != Key::Unknown) {
            queue_input(InputEvent{id, KeyReleased{mapped}});
        }
    }

    void handle_button(const x11::XButtonEvent& button, bool pressed) {
        WindowId id;
        if (!lookup(button.window, id)) {
            return;
        }
        // Wheel steps arrive as presses of buttons 4 to 7; their releases carry nothing.
        if (const auto wheel = wheel_from_x11_button(button.button)) {
            if (pressed) {
                queue_input(InputEvent{id, *wheel});
            }
            return;
        }
        if (const auto mouse_button = mouse_button_from_x11(button.button)) {
            if (pressed) {
                queue_input(InputEvent{id, MouseButtonPressed{*mouse_button}});
            } else {
                queue_input(InputEvent{id, MouseButtonReleased{*mouse_button}});
            }
        }
    }

    // Raw motion is selected on the root window, so it arrives whatever window has focus. It goes to the focused window,
    // as Raw Input does on Win32. Valuators 0 and 1 are X and Y; raw_values holds only the valuators that are set.
    void handle_raw_motion(x11::XGenericEventCookie& cookie) {
        if (!xi_opcode || cookie.extension != xi_opcode || cookie.evtype != x11::kXIRawMotion || grabbing_window) {
            return;  // a grabbed pointer's motion comes from handle_pointer_motion instead
        }
        if (!api->XGetEventData(display, &cookie)) {
            return;
        }
        const auto* raw = static_cast<const x11::XIRawEvent*>(cookie.data);
        double deltas[2] = {0.0, 0.0};
        std::size_t next = 0;
        for (int valuator = 0; valuator < 2 && valuator < raw->valuators.mask_len * 8; ++valuator) {
            if ((raw->valuators.mask[valuator / 8] >> (valuator % 8)) & 1U) {
                deltas[valuator] = raw->raw_values[next++];
            }
        }
        api->XFreeEventData(display, &cookie);

        const auto dx = static_cast<std::int32_t>(deltas[0]);
        const auto dy = static_cast<std::int32_t>(deltas[1]);
        if (focused_window && (dx != 0 || dy != 0)) {
            queue_input(InputEvent{*focused_window, MouseMoved{dx, dy}});
        }
    }

    [[nodiscard]] bool lookup(x11::Window window, WindowId& id) const {
        const auto found = ids_by_window.find(window);
        if (found == ids_by_window.end()) {
            return false;
        }
        id = found->second;
        return true;
    }

    // Only records state and queues events; no application code runs here.
    void handle_event(x11::XEvent& event) {
        switch (event.type) {
            case x11::kClientMessage: {
                // The window manager's close button sends WM_DELETE_WINDOW. The window stays open until the
                // application calls destroy_window().
                const x11::XClientMessageEvent& message = event.xclient;
                WindowId id;
                if (message.message_type == wm_protocols && message.format == 32 &&
                    message.data.l[0] == static_cast<long>(wm_delete_window) && lookup(message.window, id)) {
                    events.push(WindowEvent{id, CloseRequested{}});
                }
                return;
            }

            case x11::kConfigureNotify: {
                const x11::XConfigureEvent& configure = event.xconfigure;
                WindowId id;
                if (!lookup(configure.window, id)) {
                    return;
                }
                WindowRecord* record = windows.find(id);
                // A minimized window keeps its empty framebuffer until it is mapped again.
                if (record->minimized) {
                    return;
                }
                const FramebufferSize size{static_cast<std::uint32_t>(configure.width),
                                           static_cast<std::uint32_t>(configure.height)};
                if (size == record->framebuffer_size) {
                    return;  // a move without a resize
                }
                record->framebuffer_size = size;
                record->logical_size = logical_from_framebuffer(size, record->dpi_scale);
                events.push(WindowEvent{id, WindowResized{record->logical_size, record->framebuffer_size}});
                return;
            }

            case x11::kUnmapNotify: {
                WindowId id;
                if (!lookup(event.xany.window, id)) {
                    return;
                }
                WindowRecord* record = windows.find(id);
                if (record->minimized) {
                    return;
                }
                // The application never unmaps its own windows, so an unmap means the window manager minimized it
                // (or moved it off the current workspace). The client area is reported as empty, as on Windows.
                record->minimized = true;
                record->framebuffer_size = {};
                record->logical_size = {};
                events.push(WindowEvent{id, WindowMinimized{}});
                events.push(WindowEvent{id, WindowResized{record->logical_size, record->framebuffer_size}});
                return;
            }

            case x11::kMapNotify: {
                WindowId id;
                if (!lookup(event.xany.window, id)) {
                    return;
                }
                WindowRecord* record = windows.find(id);
                if (!record->minimized) {
                    return;
                }
                record->minimized = false;
                events.push(WindowEvent{id, WindowRestored{}});

                x11::Window geometry_root = 0;
                int x = 0;
                int y = 0;
                unsigned int width = 0;
                unsigned int height = 0;
                unsigned int border = 0;
                unsigned int depth = 0;
                api->XGetGeometry(display, record->window, &geometry_root, &x, &y, &width, &height, &border, &depth);
                record->framebuffer_size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
                record->logical_size = logical_from_framebuffer(record->framebuffer_size, record->dpi_scale);
                events.push(WindowEvent{id, WindowResized{record->logical_size, record->framebuffer_size}});
                return;
            }

            case x11::kFocusIn:
            case x11::kFocusOut: {
                // Grabs (menus, drags) do not move keyboard focus. A change to a child window (Inferior) keeps focus inside
                // this window. Pointer-root focus moves with the pointer, so NotifyPointer and NotifyPointerRoot do move it.
                const x11::XFocusChangeEvent& focus = event.xfocus;
                if (focus.mode != x11::kNotifyNormal || focus.detail == x11::kNotifyInferior ||
                    focus.detail == x11::kNotifyDetailNone) {
                    return;
                }
                WindowId id;
                if (!lookup(focus.window, id)) {
                    return;
                }
                WindowRecord* record = windows.find(id);
                record->focused = event.type == x11::kFocusIn;
                if (record->focused) {
                    focused_window = id;
                    if (record->input_context != nullptr) {
                        api->XSetICFocus(record->input_context);
                    }
                    if (record->captured && !record->minimized) {
                        apply_grab(id);
                    }
                } else {
                    // Keys and buttons held when focus leaves have no release to come, so release them here.
                    if (focused_window && *focused_window == id) {
                        focused_window.reset();
                    }
                    if (record->input_context != nullptr) {
                        api->XUnsetICFocus(record->input_context);
                    }
                    release_grab(id);
                    release_held_input(id);
                }
                events.push(WindowEvent{id, WindowFocusChanged{record->focused}});
                return;
            }

            case x11::kKeyPress:
                handle_key(event.xkey, true);
                return;

            case x11::kKeyRelease:
                handle_key(event.xkey, false);
                return;

            case x11::kButtonPress:
                handle_button(event.xbutton, true);
                return;

            case x11::kButtonRelease:
                handle_button(event.xbutton, false);
                return;

            case x11::kMotionNotify:
                handle_pointer_motion(event.xmotion);
                return;

            case x11::kGenericEvent:
                handle_raw_motion(event.xcookie);
                return;

            default:
                return;
        }
    }

    void destroy_now(WindowId id) {
        WindowRecord* record = windows.find(id);
        if (record == nullptr) {
            return;
        }
        if (grabbing_window && *grabbing_window == id) {
            release_grab(id);
        }
        if (focused_window && *focused_window == id) {
            focused_window.reset();
        }
        if (record->input_context != nullptr) {
            api->XDestroyIC(record->input_context);
        }
        api->XDestroyWindow(display, record->window);
        api->XFlush(display);
        ids_by_window.erase(record->window);
        windows.erase(id);
    }
};

core::Result<std::unique_ptr<Platform>, PlatformError> Platform::create() {
    const char* load_error = nullptr;
    const x11::Api* api = x11::load_api(&load_error);
    if (api == nullptr) {
        return core::failure(PlatformError{PlatformErrorCode::NoDisplay,
                                           std::string("the X11 client library is unavailable: ") + load_error});
    }

    x11::Display* display = api->XOpenDisplay(nullptr);
    if (display == nullptr) {
        const char* display_name = std::getenv("DISPLAY");
        return core::failure(PlatformError{
            PlatformErrorCode::NoDisplay,
            std::string("cannot open the X display '") + (display_name != nullptr ? display_name : "(DISPLAY unset)") +
                "'; start an X server or XWayland, or use CPU-only tools that do not need a window"});
    }

    auto impl = std::make_unique<Impl>();
    impl->api = api;
    impl->display = display;
    impl->owner = std::this_thread::get_id();
    impl->screen = api->XDefaultScreen(display);
    impl->root = api->XDefaultRootWindow(display);
    g_api = api;
    impl->previous_error_handler = api->XSetErrorHandler(&handle_x_error);

    impl->wm_protocols = api->XInternAtom(display, "WM_PROTOCOLS", 0);
    impl->wm_delete_window = api->XInternAtom(display, "WM_DELETE_WINDOW", 0);
    impl->net_wm_name = api->XInternAtom(display, "_NET_WM_NAME", 0);
    impl->utf8_string = api->XInternAtom(display, "UTF8_STRING", 0);

    // Xft.dpi is the user's text scale (96 at 100 percent). It is read once per Platform; see docs/windowing.md.
    std::uint32_t dpi = kBaseDpi;
    if (const char* value = api->XGetDefault(display, "Xft", "dpi")) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && parsed > 0 && parsed < 10000) {
            dpi = static_cast<std::uint32_t>(parsed);
        }
    }
    impl->dpi_scale = dpi_scale_from_dpi(dpi);

    // Auto-repeat arrives as presses only, as on Win32. Servers without XKB still send release-press pairs, which
    // pump_events folds into nothing.
    x11::Bool detectable_supported = 0;
    api->XkbSetDetectableAutoRepeat(display, 1, &detectable_supported);

    // The input method provides composed text. Without one, text comes from keysyms and dead keys do not compose.
    api->XSetLocaleModifiers("");
    impl->input_method = api->XOpenIM(display, nullptr, nullptr, nullptr);
    if (impl->input_method == nullptr) {
        core::log_message(core::LogLevel::Info, "platform",
                          "no X input method: text comes from keysyms, and dead keys do not compose");
    }

    // XInput2 supplies raw relative motion. Without it the backend never reports MouseMoved, and the log says so.
    const char* xi_error = nullptr;
    impl->xi = x11::load_xi_api(&xi_error);
    x11::Bool xi_event = 0;
    x11::Bool xi_error_code = 0;
    if (impl->xi == nullptr) {
        core::log_message(core::LogLevel::Warning, "platform",
                          std::string("relative mouse motion is unavailable: ") + xi_error);
    } else if (api->XQueryExtension(display, "XInputExtension", &impl->xi_opcode, &xi_event, &xi_error_code) != 0) {
        int major = 2;
        int minor = 2;
        if (impl->xi->XIQueryVersion(display, &major, &minor) == x11::kSuccess && major >= 2) {
            unsigned char raw_motion_mask[4] = {};
            raw_motion_mask[x11::kXIRawMotion / 8] |= static_cast<unsigned char>(1U << (x11::kXIRawMotion % 8));
            x11::XIEventMask select{x11::kXIAllDevices, static_cast<int>(sizeof(raw_motion_mask)), raw_motion_mask};
            impl->xi->XISelectEvents(display, impl->root, &select, 1);
        } else {
            impl->xi_opcode = 0;
            core::log_message(core::LogLevel::Warning, "platform", "XInput2 2.0 is not available: relative mouse motion is unavailable");
        }
    } else {
        core::log_message(core::LogLevel::Warning, "platform", "the X server has no XInput extension: relative mouse motion is unavailable");
    }

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
    return impl_->minimize_window(id);
}

core::Result<core::Unit, PlatformError> Platform::restore_window(WindowId id) {
    return impl_->restore_window(id);
}

core::Result<WindowInfo, PlatformError> Platform::window_info(WindowId id) const {
    return impl_->window_info(id);
}

core::Result<Win32WindowHandles, PlatformError> Platform::win32_handles(WindowId) const {
    return core::failure(PlatformError{PlatformErrorCode::Unsupported,
                                       "Win32 handles exist only on the Win32 backend; use xlib_handles"});
}

core::Result<XlibWindowHandles, PlatformError> Platform::xlib_handles(WindowId id) const {
    return impl_->xlib_handles(id);
}

void Platform::pump_events() {
    impl_->pump_events();
}

std::vector<WindowEvent> Platform::drain_events() {
    impl_->require_owner();
    return impl_->events.drain();
}

void Platform::set_modal_tick(ModalTick tick) {
    impl_->require_owner();
    impl_->modal_tick = std::move(tick);
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
