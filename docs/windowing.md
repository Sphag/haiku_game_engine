# Windowing

The `engine_platform` library (`engine/platform/`) owns native windows. It is in-house: Win32 is called directly
through `user32`, and X11 is spoken directly through `libX11.so.6`, with no SDL or GLFW. Windows (US-007) and Linux/X11
(US-008) are implemented. Other operating systems return `Unsupported`, so no fake window is ever created.

Input (keyboard, mouse, text, gamepad, cursor capture) is in the same library and is described in `docs/input.md`.
Windows input is US-009; Linux input is US-010.

## API

`engine/platform/include/engine/platform/platform.hpp` is the entry point.

- `Platform::create()` enables per-monitor DPI awareness v2 for the process and registers the window class. Create
  one `Platform` per process and use it only on the thread that created it. Calls from another thread assert.
- `create_window(WindowDesc)` returns a `WindowId`. The id is generation-checked. After `destroy_window`, every later
  call with that id returns `NotFound`, even if the slot is reused.
- `pump_events()` dispatches pending OS messages. `drain_events()` returns the queued `WindowEvent`s in order.
- `win32_handles(id)` returns the `HINSTANCE` and `HWND` for DX12 swapchains or Vulkan Win32 surfaces (Windows).
- `xlib_handles(id)` returns the `Display*` and `Window` for a Vulkan Xlib surface (Linux). The other backend returns
  `Unsupported` for it.

## Sizes

Two sizes are kept apart on purpose:

| Size | Type | Meaning |
|---|---|---|
| Framebuffer | `FramebufferSize` (pixels) | Client area in physical pixels. Swapchains and render targets use this. |
| Logical | `LogicalSize` (DIP) | Framebuffer divided by `dpi_scale`. Layout and UI use this. |

`dpi_scale` is `DPI / 96`. The framebuffer is the logical size times the scale, rounded to the nearest pixel. A
window requested at 1280x720 DIP and 150% has a 1920x1080 framebuffer.

## Events

| Event | When |
|---|---|
| `CloseRequested` | The user closed the window. The window stays open until the application calls `destroy_window`. |
| `WindowResized` | The client size changed. Consecutive resizes of one window are merged into the latest one. |
| `WindowMinimized` / `WindowRestored` | The window was minimized or came back from minimized. |
| `WindowFocusChanged` | The window gained or lost focus. |
| `WindowDpiChanged` | The window moved to a monitor with a different scale. The OS suggested rectangle keeps the logical size. |

## Zero size and destruction

- A minimized window has a 0x0 framebuffer and a 0x0 logical size. Renderers must skip presenting and must not
  create swapchains with zero dimensions.
- `destroy_window` called while a message is being dispatched takes effect when that dispatch returns. The
  window is never freed while its message handler is on the stack.
- During a drag resize, the OS holds the thread in its own loop and `pump_events()` does not return. The modal tick
  set by `set_modal_tick` runs about every 16 ms inside that loop so rendering continues. The tick may create and
  destroy windows and drain events. It must not call `pump_events()`.
- A C++ exception thrown by application code in a message or the tick is captured and rethrown by `pump_events()`.
  Exceptions cannot cross the OS callback.

## Linux (X11)

`engine/platform/src/x11/` holds the backend. It uses only the Xlib functions it needs, declared in
`xlib_abi.hpp` for LP64 Linux and bound at run time with `dlopen("libX11.so.6")`.

- **No X11 headers or `-dev` packages are needed to build.** The build links only `${CMAKE_DL_LIBS}`. A machine
  without `libX11.so.6` still builds and runs its CPU code.
- **Missing display:** `Platform::create()` returns `NoDisplay`, with the reason in `detail` (the library is missing,
  or `DISPLAY` is unset or refused). No window is created. CPU-only code does not need a `Platform`.
- **XWayland** is an X server, so windows made here run under XWayland. On WSLg, `DISPLAY` points at XWayland, and
  its window manager is Weston. Native X11 sessions need the same code path; they are not verified yet (see below).
- Protocol errors go to the log, not to the default Xlib handler, which exits the process. `create_window` checks
  for an error after its requests and fails with `CreateFailed` if one occurred.

Differences from the Win32 backend, which follow from X11 and are part of the contract on Linux:

| Area | Linux/X11 behaviour |
|---|---|
| Close | `WM_DELETE_WINDOW` produces `CloseRequested`. The window stays open until `destroy_window()`. |
| Resize | `ConfigureNotify` with a changed size produces `WindowResized`. The framebuffer is the X window's pixel size. |
| Minimize | An unmap of a window the application did not unmap is the minimize signal. The framebuffer and logical sizes become 0x0 and `WindowMinimized` and `WindowResized` (empty) follow. Mapping it again produces `WindowRestored` and `WindowResized` with the real size. |
| Focus | `FocusIn` and `FocusOut` in normal mode update `focused`. Grab focus changes and pointer-only changes are ignored. |
| DPI | The scale is `Xft.dpi / 96`, read when the `Platform` is created. Changes at run time are not observed, so no `WindowDpiChanged` is sent. |
| Modal tick | Never runs. X11 has no modal size loop, so the frame loop keeps running during a drag. |
| Destroy | Takes effect at once. Application code never runs inside an X event, so there is no deferral. |
| Non-resizable | `WM_NORMAL_HINTS` pins the minimum and maximum size. |
| Titles | Sent as UTF-8 in `_NET_WM_NAME`. |

The process-wide Xlib error handler is installed by `Platform::create()` and restored when the `Platform` is destroyed.
Xlib is not initialised for threads (`XInitThreads`). Every call must come from the thread that created the
`Platform`, as on Windows. A Vulkan surface created on another thread will need `XInitThreads` when the RHI lands.

## Tests

- `tests/src/platform_window_tests.cpp` covers the platform-neutral parts on every OS: id generation, DPI conversion,
  title and size validation, event coalescing, and the Linux `Unsupported` result.
- `tests/src/platform_x11_tests.cpp` (Linux only) uses a real X server. It covers the missing-display `NoDisplay` result,
  multiple windows with server-side checks, close via `WM_DELETE_WINDOW`, resize, the minimize and restore
  semantics, focus, and re-creating a `Platform`. Tests needing a display report `SKIP` when `DISPLAY` is not
  reachable. Minimize, resize, restore, and focus that depend on the window manager also report `SKIP` when it does not
  honour the request. The rule is the same for each: the server state decides. If the server did not reach the
  requested state, the window manager refused it and the test skips with the reason. If the server did reach it and
  the backend did not report it, the test fails. The synthetic focus test sends focus events and needs no window
  manager. The synthetic unmap test needs the window manager to show the window first, because the server sends no
  unmap event for a window whose frame is not mapped; it skips otherwise. The unmap tests check the events that
  arrive, not the window's state afterwards, since a window manager may remap a window right away.
- `tests/src/platform_win32_tests.cpp` creates real windows and covers multiple windows, close, resize,
  minimize/restore, focus, DPI change, destruction from the modal tick, and exception propagation. It runs only on
  Windows and needs a desktop session.

Focus and DPI are checked on Windows by sending the messages the OS would send. Real keyboard focus and a real monitor change
are not automated. They are part of the manual GUI check, which is not run yet because no application window exists
in the repository.
