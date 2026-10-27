# Input

Input lives in `engine_platform` next to windowing (`engine/platform/`). It is in-house: no SDL, GLFW, or other input
library. Windows uses Win32 Raw Input, the keyboard and text window messages, and XInput (US-009). Linux uses X11 core
events, XInput2 raw motion (`libXi`), XIM text (`libX11`), and evdev gamepads (US-010). The system libraries are bound
at run time with `dlopen`, so no X11 headers or `-dev` packages are needed to build.

## API

- `Platform::drain_input_events()` returns the input events since the last call, in order.
- `Platform::set_cursor_capture(id, captured)` and `cursor_captured(id)` control capture per window.
- `InputState` turns events into per-frame states. Call `begin_frame()`, then `apply()` each event of that frame.
  `key_down` is held state, `key_pressed` and `key_released` are the edges of this frame. The same holds for mouse
  buttons and gamepad buttons.
- `input_mapping.hpp` holds the pure conversions: virtual key to `Key`, X11 keysym to `Key`, keysym to code point,
  X11 button and wheel to mouse events, XInput bit to pad button, UTF-16 to code points, UTF-8 encoding, control
  character removal, stick dead zones, trigger thresholds, and gamepad diffs. They take plain integers and build on
  every OS.
- `evdev.hpp` holds the pure evdev decoding: record parsing, button and axis codes, stick and trigger normalization,
  the D-pad hat, the gamepad test, the permission diagnostic, and slot assignment. It also builds on every OS.

## Actions

`action_map.hpp` binds named game controls to devices, so controls change without code (US-011). It is pure and builds
on every OS.

- An action is a **button** (down while its combined value is at least 0.5) or an **axis** (its combined value,
  clamped to [-1, 1]). A button reads 1 when down and 0 otherwise.
- A binding reads one source: a key, a mouse button, mouse motion or the wheel (`MotionX`, `MotionY` in device counts,
  `WheelX`, `WheelY` in notches), a gamepad button, or a gamepad axis. A gamepad source names a slot, or reads any
  connected pad when it names none.
- A binding contributes `(source value, minus its dead zone, rescaled) * scale`. The dead zone applies to gamepad axes
  only, in [0, 1). The rescale keeps full deflection at 1. A scale of -1 makes a key or stick negative, so `D` (+1) and
  `A` (-1) form an axis.
- Several bindings of one action combine in the same frame. A keyboard key, a mouse button, and a pad button can drive
  one action together. Their contributions add and then clamp.
- `ActionMap` holds the authored defaults. `rebind` replaces one action's bindings for this user, `reset` and `reset_all`
  return to the defaults, and an empty `rebind` unbinds the action. `add_action`, `rebind`, and `apply_overrides` reject
  an invalid binding and change nothing.
- `ActionEvaluator::update(map, gameplay_input)` runs once per frame and gives each action its value and its down,
  pressed, and released edges.

### Profiles

`action_profile.hpp` saves the user's overrides as JSON. Only the overrides are saved, so an action the user did not
change keeps its authored defaults. The file carries `"format": "engine.input_profile"` and `"version": 1`. `parse_profile`
rejects another format name, another version, bad JSON, and unknown source or key names, and names the action at fault.
`apply_overrides` checks the bindings' values, and it replaces all overrides or none. Saving uses `write_file_atomic`.

## Routing

`input_router.hpp` sits between the platform events and gameplay (US-011).

- Editor and UI owners request capture per device class: keyboard (keys and text), mouse, or gamepad. Each owner's
  request is kept apart, and the effective capture is their union.
- `route(events)` sends captured events to `captured_events()`. Everything else reaches `gameplay_events()` and
  `gameplay_input()`.
- Gameplay's state follows the devices. A capture that starts while a key is held releases it for gameplay. A capture
  that ends while the key is still held presses it for gameplay. A captured pad reads as disconnected with its axes at
  rest, and it reconnects with its held buttons and stick positions when the capture ends.
- `device_input()` is the physical state, whatever is captured. Editor tools read it to show what is held.
- `set_mode(RouterMode::Play)` releases every capture, and capture requests are refused until `Edit` returns. Gameplay
  then gets the held devices on the first frame of Play.

Limits of this stage: no editor or game UI exists yet, so capture is exercised with synthetic events and no window. The
frame loop does not call `route()` yet; the runtime (US-060) and editor (US-061) call it once per frame. In Play, nothing
is captured, so editor controls such as Stop cannot take the mouse from gameplay. Their input path is decided with the
Play controls (US-065).

## Events

| Event | Windows source | Linux source | Notes |
|---|---|---|---|
| `KeyPressed`, `KeyReleased` | `WM_KEYDOWN`/`WM_KEYUP` and `WM_SYS*` | X11 `KeyPress`/`KeyRelease` | Mapped to `Key`. Left and right modifiers are separate keys. Auto-repeat is dropped (Linux: detectable auto-repeat, with a release-press pair folded as a fallback). |
| `TextInput` | `WM_CHAR` | XIM `Xutf8LookupString`, or the keysym when no input method is open | One UTF-8 string per key press. Control characters are dropped. Chords with Ctrl, Alt, or Super type nothing. |
| `MouseMoved` | Raw Input mouse | XInput2 raw motion, or pointer motion while captured | Relative device counts. |
| `MouseButtonPressed`/`Released` | Raw Input mouse | X11 buttons 1, 2, 3, 8, 9 | Left, right, middle, X1, X2. |
| `MouseWheelScrolled` | Raw Input mouse | X11 buttons 4 to 7 | Notches. Vertical is positive away from the user. Horizontal is positive to the right. |
| `GamepadConnected`/`Disconnected` | XInput polling | evdev rescan | Windows checks once per `pump_events()`. Linux rescans `/dev/input` every 500 ms. |
| `GamepadButtonPressed`/`Released`, `GamepadAxisChanged` | XInput polling | evdev records | Sticks are in [-1, 1] with +Y up. Triggers are in [0, 1]. |

Windows keyboard and text come from window messages, and the mouse comes from Raw Input, so each kind has one source. A
raw keyboard registration would report every key a second time. `RegisterRawInputDevices` uses a null target, so the
mouse follows keyboard focus.

On Linux, XI2 raw motion is selected on the root window, so it reaches every window. It is delivered to the focused
window, as Raw Input is on Windows.

Every event is checked against the current state before it is queued. Pressing a held key, releasing a key that is
up, or connecting a connected pad is dropped, so no frame contains a duplicate transition.

## Focus and capture

- Losing focus queues a release for every held key and mouse button of that window, then clears the text decoder. The
  key-up that may arrive later adds nothing.
- Gamepads are polled and do not depend on focus, so they are not released on focus loss. Their state is always the
  current hardware state.
- Cursor capture is requested per window and applied only while the window is focused and not minimized.
  - Windows: the cursor is hidden over the client area and confined to it with `ClipCursor`. The clip follows the window
    on move and resize.
  - Linux: the pointer is warped to the centre of the window, then grabbed (`XGrabPointer`, confined to the window) with
    an empty cursor. Captured motion is the offset of each pointer event from the last reported position. Near an edge
    the pointer is warped back to the centre. A single step larger than the window is a server reposition, so it is not
    reported as motion.
  - Capture is released on focus loss, on capture off, and on destruction.
- Relative motion comes from Raw Input on Windows. On Linux it comes from XI2 raw motion while the pointer is free.
  While captured it comes from pointer motion, because raw motion did not arrive during the grab on XWayland.

## Linux gamepads and permissions

- Gamepads are read from `/dev/input/event*`. A node counts as a gamepad when it has `BTN_SOUTH` and the left stick's
  X axis. Keyboards, mice, and touch devices are skipped.
- Each gamepad takes the lowest free slot (0 to 3). A device that is unplugged reads as disconnected, so its buttons are
  released and `GamepadDisconnected` is reported.
- Stick dead zones are the larger of the device's own flat value and 24 percent of the half range, the XInput default.
  Triggers under 12 percent read as 0.
- The D-pad is a hat (`ABS_HAT0X`/`ABS_HAT0Y`). Returning the hat to centre releases the D-pad button.
- **Permissions:** a node that cannot be opened for permission reasons is logged once with the fix:
  `sudo usermod -aG input $USER`, then log in again. Other failures name the path.
- **No devices:** when `/dev/input` is missing (WSL and some containers have none), the log says gamepads are
  unavailable there. Keyboard and mouse still work.

## Limits

- **Modal drag:** during the OS move/size loop on Windows, `pump_events()` does not return. Mouse and gamepad input are
  not polled until the drag ends. The modal tick cannot poll, because it must not call `pump_events()`. X11 has no modal
  loop.
- **Keyboard layout:** keys are mapped from the unshifted keysym (X11) or the virtual key (Windows), so they follow the
  active layout. A layout with no Latin letter at a key's position falls back to the group that has one. Physical-
  position mapping is not implemented.
- **Composition:** on Windows only committed characters from `WM_CHAR` arrive. On Linux the input method composes text
  when one is open. Without one, dead keys do not compose. XIM uses the process locale (`XSetLocaleModifiers("")`).
- **Native Wayland:** not implemented. Linux runs on X11 or XWayland, as the PRD's Non-Goals state.
- **XWayland and grabs:** raw motion did not arrive while the pointer was grabbed. Captured motion therefore uses pointer
  motion and warps, so it is measured in window pixels, not device counts.
- **Dead zones** in the backend are the XInput defaults on Windows and the evdev defaults on Linux. An action binding
  adds its own dead zone on top, in the Actions section.
- **Hotplug latency:** a connection is seen on the next `pump_events()` after it happens. Linux rescans every 500 ms.

## Tests

- `tests/src/platform_input_tests.cpp` (all OS, `unit`): replay of pressed, held, and released edges across frames;
  duplicate suppression within a frame; focus-loss release order; Unicode and UTF-16 decoding; mouse accumulation;
  gamepad diffs and unplug; dead zones; virtual-key mapping.
- `tests/src/platform_replay_tests.cpp` and `tests/fixtures/input/shared_replay.txt` (all OS, `unit`): one input sequence
  written with Win32 and X11 codes. Each line goes through its OS's pure translation, then the same `InputState` and
  `diff_gamepad`. Both must produce the expected normalized events, and the held state must match. Also covers the X11
  keysym, button, and control-character mappings.
- `tests/src/platform_evdev_tests.cpp` (all OS, `unit`): record parsing, code tables, stick and trigger normalization,
  dead zones, the D-pad hat, gamepad detection, the permission and missing-device messages, and slot assignment.
- `tests/src/platform_win32_input_tests.cpp` (Windows): key, text, and focus messages through the real window
  procedure with `SendMessage`; raw mouse report decoding from synthetic `RAWMOUSE` structs; cursor clip on focus,
  capture off, and destroy.
- `tests/src/input_action_tests.cpp` (all OS, `unit`): button and axis edges, key pairs, one action driven by keyboard,
  mouse, and pad together, slot selection, stick dead zones and their scale, the trigger threshold, mouse motion and
  wheel, remapping, unbinding, reset, invalid definitions and bindings, and the name tables.
- `tests/src/input_profile_tests.cpp` (all OS, `unit`): profile round trip with stable text, defaults kept for untouched
  actions, format, version, and syntax rejection, unknown names, all-or-nothing apply, and file save and load.
- `tests/src/input_router_tests.cpp` (all OS, `unit`): captured input stays out of gameplay, the held-state
  reconciliation when a capture starts and ends, the gamepad capture and restore, separate owners, Play releasing
  capture, and the physical state during capture.
- `tests/src/input_scenario_tests.cpp` (all OS, `integration`): scripted sessions. Keyboard, pad, and mouse drive the
  same actions in the same frames. A remap and a loaded profile change controls mid-session. An editor capture is
  followed by Play. Each frame's action values are checked.
- `tests/src/platform_x11_input_tests.cpp` (Linux, `integration`): injects input with XTest into a real X server.
  Checks key press, text, and release; relative motion; buttons and wheel; focus loss releasing a held key; cursor
  capture; and the no-`/dev/input` path. A test SKIPs with a reason when there is no display, when XTest is missing, or
  when the server does not show the window or give it focus. The window manager decides those, so a SKIP is not a pass.

Not automated: a physical controller, a physical keyboard focus change, a real raw mouse device, a permission-denied
device node, and hotplug on a real `/dev/input`. These need hardware or a real evdev node, which this environment does
not have. The manual GUI check is not run either, because no application window exists in the repository yet.
