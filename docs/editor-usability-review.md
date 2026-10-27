# Editor usability review

Date: 2026-10-27. Method: a heuristic review (Nielsen's ten, plus "first hour" and "daily use" walk-throughs) of the editor as it ran on the
Windows reference machine, using the showcase project. Every finding below was seen in a screenshot of the real window or on the way to one;
the "after" column was checked the same way. The real-input test `tools/ui_tests/editor_ui_test.ps1` repeats the key flows.

Scale: **Blocker** (a task cannot be done), **Major** (done, but slowly or with a workaround), **Minor** (friction), **Polish**.

## Summary

| Area | Before | After |
|---|---|---|
| First launch and opening a project | Major: a folder path had to be typed or pasted by hand | Native folder chooser, recent list, welcome state |
| Typing text anywhere | **Blocker**: no text reached any field with a real keyboard | Fixed (the message pump never translated key presses) |
| Opening a scene | Major: type an absolute path into a box on another tab | File > Open scene, double click in Assets, `--scene` |
| Reading the hierarchy | Major: rows were GUID prefixes | Entities have names; prefab instances show their prefab |
| Inspector | Major: raw field names, GUID text boxes, cramped | Redesigned (below) |
| Shortcuts | Minor: undo and redo only, and Ctrl never worked | Ctrl+S, D, Z, Y, Delete, W/E/R, F5; modifiers fixed |
| Build Game | Major: three typed fields, a GUID, an output path | Folder chooser, start scene picked from a list |
| Play | Major: Play ran no physics or scripts, and scripts could not move entities | Bodies, scripts, animators, characters, audio |

Overall: **before, the editor could not be used to make anything**: no text entry, no components, no way to open a scene without typing a
path, no names. **After, a first-time user can open the sample, find an entity by name, change what it is made of, press Play, and build a
game without typing a path.** It is a competent early-stage editor, not yet a comfortable one; the remaining list is at the end.

## Findings and what changed

### F1. Text could not be typed (Blocker, fixed)
The Win32 message pump dispatched `WM_KEYDOWN` but never called `TranslateMessage`, so no `WM_CHAR` was produced. The Folder box, the search
box, and every field were dead to a real keyboard. Automated tests sent `WM_CHAR` by hand and hid it. Found by the first real-input run. Also
fixed in the same pass: Ctrl, Shift, and Alt never reached Dear ImGui (so Ctrl+Z and Ctrl+A did nothing), the first click on an unfocused
window was lost, and clicks aimed at another window above the editor could reach it.

### F2. Paths were typed by hand (Major, fixed)
*Heuristic: recognition over recall; error prevention.* A path is easy to mistype and hard to remember. Now:
- **Open project...** and **New project...** buttons use the operating system's folder chooser (`engine/platform/dialogs.hpp`; Windows
  `IFileOpenDialog`, Linux zenity or kdialog when installed). A text box remains under "Type a path instead" for machines with no chooser.
- **Recent projects** are listed on the Project panel and under File > Open recent, and the last project reopens at start.
- **File > Open scene** lists the project's scenes; **Open scene from file...** uses the file chooser; a double click in Assets opens a scene;
  `game_editor --project <folder> --scene <path>` opens one at start.
- **Build Game** has a Browse button for the output folder and a drop-down of the project's scenes instead of a GUID box.

### F3. Entities had no names (Major, fixed)
The hierarchy showed `0f95d3b0`. There was no way to tell the sun from the ground. Added a `String` field kind to the reflection system
(UTF-8, at most 256 bytes, no control characters), a `name` on `engine.name_tag`, a name field at the top of the inspector, and names in
the hierarchy. A prefab instance is labelled by its prefab file. The same field names animators for scripts.

### F4. The inspector was not friendly (Major, redesigned)
Before: a heading, then `InputFloat` rows with the raw field name (`cone_angle_degrees`), a text box holding a GUID for every asset, no way to
remove a component except a button at the bottom of its list, and a combo plus a button to add one.

After:
- **Header:** editable name, the GUID in small type (click to copy), and a plain statement when the entity belongs to a prefab.
- **Transform:** a three-column grid for Position, Rotation, and Scale with drag-to-change, double-click-to-type, tooltips, and Reset.
- **Component cards:** a collapsible header per component, titled in words (Mesh Renderer, not `engine.mesh_renderer`), with a remove button
  on the header.
- **Field widgets by meaning:** sliders for bounded numbers, drag fields for the rest, color swatches for colors, drop-down lists for choices
  (light kind, body motion and shape), checkboxes, and labelled fields in two aligned columns. Ranges appear in tooltips.
- **Asset fields** show the asset's file name, not a GUID: click for a searchable list of the assets that suit the field (models for a mesh,
  materials for a material, and so on), an `x` clears it, drag from the Assets panel still works, and a missing asset is shown in red.
- **Add Component** is one full-width button that opens a searchable list.
- Presentation comes from a `hint` that a component declares for each field (`color`, `enum:A|B|C`, `asset:<kind>`), so game code gets the
  same widgets without touching the editor.
- The default layout gives the inspector 40% of the right-hand area, so asset names are not cut off.

### F5. Shortcuts (Minor, fixed)
Ctrl+S saves, Ctrl+D duplicates, Delete removes, W/E/R choose the gizmo, F5 plays and stops, Ctrl+Z / Ctrl+Y undo and redo. They are
listed in the menus and in `docs/editor.md`, and they do nothing while a text field has the keyboard.

### F6. Play did not play (Major, fixed)
Play created physics and a script runner but no bodies, no script instances, no entity registry binding, no animators, no audio, and no
characters. Scripts that moved an entity raised an error. Play now runs the same `GameWorld` as the game.

### F7. Feedback and errors (Minor, partly fixed)
The console and the Errors panel exist and every command reports its outcome there, which is good. Added `--log <file>` so the same text can
be read by scripts. A failed rename or field edit shows its reason under the inspector.

## Remaining issues

| # | Severity | Issue | Suggested fix |
|---|---|---|---|
| R1 | Major | The viewport draws entity markers and a gizmo, not the scene. A person cannot see what they are editing without pressing Play in a game window. | Render the edit viewport with `GameRenderer` into a texture shown in the panel |
| R2 | Major | The Hierarchy lives in a tab beside Project and Assets, so selecting an entity means changing tabs | Make the Hierarchy its own default dock |
| R3 | Minor | No right-click menus (rename, duplicate, delete, add child) on hierarchy rows; no drag-to-reorder | Context menus |
| R4 | Minor | No rename by double click or F2 in the hierarchy; names are edited in the inspector | Inline rename |
| R5 | Minor | The Assets panel lists a flat catalog with paths, no folders or thumbnails except for textures | Folder tree and icons |
| R6 | Minor | No unsaved-changes marker in the window title or the panel title | `*` in the title |
| R7 | Minor | Mouse cursor shapes never change (resize, text, hand) | Platform cursor API |
| R8 | Minor | The window does not redraw while it is being resized or moved | Use the platform's modal tick |
| R9 | Minor | A material graph, animation graph, terrain, and world editor exist but are opened from the View menu with no hint that they exist | Put them in a "Windows" section with icons, and in the welcome panel |
| R10 | Polish | No dark/light choice, no UI scale control (it follows the display's DPI), and the font is fixed | Settings panel |
| R11 | Polish | No undo history list | History panel |
| R12 | Polish | Linux: the folder chooser needs zenity or kdialog; the X11 path was not re-checked for the mouse and focus fixes made on Windows | Test on a native Linux desktop |

## How this was checked
- Screenshots of the real window at each step: `artifacts/ui/` after a run of `tools/ui_tests/editor_ui_test.ps1`.
- That script uses real mouse and keyboard input (`mouse_event`, `keybd_event`, SendKeys) to: choose a folder in the native chooser, open the
  project, open a scene, rename an entity in the inspector, duplicate, undo, redo, delete, save, check the saved file, and Play and stop.
- Headless tests in `tests/src/editor_*` cover the model and the ImGui adapter.
