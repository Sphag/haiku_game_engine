# Editor shell (US-061)

The editor is one native window with a docking layout. It opens and creates projects, shows a console and the errors, and saves its
workspace. The code is in `engine/editor/` (the library) and `tools/game_editor/` (the executable). The runtime does not link either
one; `engine/runtime/CMakeLists.txt` checks that at configure time.

## Layers

- **Document model** (`editor_model.hpp`, `project.hpp`, `workspace.hpp`, `console.hpp`). Plain C++ with no graphics or ImGui. Every
  command (open, create, close, save, reset layout, panel visibility) writes its outcome to the console, so a failure is visible in the
  Errors panel and the editor keeps running. The unit tests drive this layer directly.
- **Platform/input adapter** (`imgui_input.hpp`). Keys, text, mouse buttons, and wheel notches from the in-house platform layer go into the
  ImGui IO queue. Relative mouse motion is not used: the pointer is read with `Platform::pointer_position` once per frame, and it is
  reported only while the window has focus.
- **Renderer** (`imgui_renderer.hpp`). Draws the UI through `engine_rhi`, so one code path serves DX12 and Vulkan. The vendored
  `imgui_impl_dx12` and `imgui_impl_vulkan` backends are not compiled: they need native handles that the RHI does not expose. The
  renderer expands each ImGui draw into one vertex per index and carries the clip rectangle in each vertex, because the RHI has no
  per-draw scissor; the pixel shader discards outside it. The font atlas is read with `Load`, so no sampler is needed.
- **Shell UI** (`editor_ui.hpp`). A menu bar, a dock space with a default layout, and the Project, Viewport, Console, Errors, and Workspace
  panels. The UI holds only its text fields; everything else is in the model.

## Projects

A project is a folder with `project.json`:

```json
{ "format": "engine.project", "version": 1, "name": "Проект" }
```

The file must hold exactly those three keys. The `assets` folder beside it is scanned with `engine::assets::scan_project`. A project
that does not open is `Invalid` with its reason, and the editor stays usable. A missing `assets` folder leaves the project open and says
so in the Project panel. Sources without a sidecar and sidecars without a source are listed, not fatal.

## Workspace

The workspace file (`--workspace`, default `editor-workspace.json` in the current folder) holds the last project, the panel visibility,
and the docking layout as ImGui's own ini text. A missing file means the defaults. A damaged file is logged and the defaults are used.
The file is written with `write_file_atomic`, so a failed save leaves the previous one.

## Running

```
game_editor [--backend dx12|vulkan] [--project <folder>] [--workspace <file>] [--font <file>] [--frames <n>] [--debug-layers]
```

The backend defaults to DX12 on Windows and Vulkan on Linux. The shaders are read from the build's `shaders` folder, so run the
executable from a build that compiled them. The font defaults to the bundled Roboto, which has the Cyrillic range. A font that cannot
be read is logged and ImGui's built-in font is used.

## Scene editing (US-062)

- `SceneEditor` (`scene_editor.hpp`) holds the scene being edited, the component catalog and prefab library it reads, the selection,
  and the scene file. It has no ImGui dependency. `EditorModel::scene()` gives the editor its instance.
- Structural edits (create, duplicate, remove, reparent, add or remove a component) change a copy of the scene document and load it
  back with `Scene::load`, which checks the whole document first. A rejected edit leaves the scene as it was.
- Field and transform edits go straight to the scene, which checks them against the reflected descriptor (type, range, finite).
  The inspector shows the rejection in its status line and the console logs it as a warning.
- Duplicate copies an entity and its descendants under the same parent with new GUIDs. Remove deletes the subtree. Reparent keeps
  the entity's local transform, so it moves with the new parent; it does not keep the world transform.
- The hierarchy filter matches GUID text or a component type ID, ignoring case. Drag an entity onto another to make it a child.
- A Guid field naming an asset the project does not hold shows its GUID with "Missing asset"; the scene still saves it.
- A scene with unsaved changes asks before New, Open, Close project, and Exit (the window's close button included).
- Known limits: the hierarchy lists the scene's own entities, not the inner entities of a prefab instance (those are edited by
  selecting them through the inspector, which needs their GUID). The scene file is typed into the Hierarchy panel; there is no file
  dialog. Prefab files are not loaded by the editor yet, so a scene with a prefab instance fails to open with the prefab's GUID.
  Every scene operation reloads the whole document, which is simple and fine for editor-sized scenes.

## Undo and redo (US-064)

- Every edit that changes the scene is one command: create, duplicate, remove, reparent, add or remove a component, a field edit, and
  a transform edit. A rejected edit is not recorded. Selection and camera moves are not commands.
- The history keeps the scene's document before and after each command. Undo loads the before-document and redo loads the
  after-document with `Scene::load`, so GUIDs, references, and components come back exactly as they were.
- The unsaved state follows the history: undoing back to the text last saved or opened makes the scene clean again, and redoing
  past it makes it dirty. A new command clears the redo steps. Opening or creating a scene clears the history.
- A gizmo drag is one command. A press opens a gesture, the drag's transform writes merge into that command, and the release closes
  it. A value edited in the inspector merges the same way while its item is active, so typing a number is one step.
- Edit menu: Undo and Redo show the command's label, for example "Undo reparent". Ctrl+Z undoes, Ctrl+Shift+Z or Ctrl+Y redoes. The
  shortcuts wait while a text field has the keyboard, which keeps its own undo.
- Known limits: the history holds a full copy of the document for each command, which is simple and fine for editor-sized scenes but
  grows with the scene size and the number of commands. It has no limit, so a long session holds all of its steps until the scene is
  opened or created again.

## Scene viewport (US-063)

The Viewport panel edits transforms in the view. Its model is `EditorViewport` (`viewport.hpp`, no ImGui or graphics dependency); the
panel reads the pointer and keys into a `ViewportInput` and draws the result.

- **Editor camera.** A free camera that orbits a target: RMB drag orbits, MMB pans, the wheel zooms, RMB with W/A/S/D/Q/E flies, and F
  frames the selection. It is not the game's camera; a gizmo drag freezes it.
- **Picking.** A press on an entity's marker selects it; a press on empty space clears the selection. The pick radius is 8 pixels at
  DPI 1. Picking tests marker positions, not mesh bounds.
- **Gizmos.** Move, Rotate, and Scale, in Local or World space. Scale always works on the entity's own axes. A drag computes the new
  local transform from the press, so it does not accumulate error. The write goes through `SceneEditor::set_transform`.
- **Snapping.** Off by default. It rounds a move to the translate step (metres), a rotation to the angle step (degrees), and a scale
  component to the scale step.
- **DPI.** The ImGui display is in framebuffer pixels, so the panel size and the pointer need no conversion. The DPI scale sizes the
  pick radius and the gizmo length, so they keep their size on screen. The host updates the scale on a `WindowDpiChanged` event.
- **Selection.** Removing the selected entity clears the selection and ends a drag. A reparent keeps the entity selected; its gizmo
  follows the new world position.

## Play mode (US-065)

Play, Pause, Step, and Stop are on the Viewport toolbar. `PlaySession` (`play_session.hpp`, no ImGui or graphics dependency) is the
runtime world, and `EditorModel` owns at most one.

- **Separate world.** Play loads the scene's document into a new `scene::Scene` with its own registry, a physics world, a script engine
  and runner, and an offline audio engine. Changes made while playing stay in that world. Stop destroys it, so the edited scene and its
  dirty state are as they were before Play. `PlaySession::live_count()` returns to zero after Stop.
- **Clock.** A playing session runs whole fixed steps (`physics::kFixedStep`, 60 Hz) for the frame time, up to the physics catch-up
  limit; time beyond it is dropped. Each fixed step updates physics and calls the scripts' `fixed_update`; each frame calls `update`.
- **Pause and Step.** Pause holds the clock and pauses physics and audio, and frame time passing while paused is not owed on resume.
  Step runs one fixed step of every system; it does nothing while the clock runs.
- **Input routing.** While a session runs, the scene is read-only: the Hierarchy and Inspector are disabled, the scene undo and redo
  are off, and the viewport keeps its camera but gets no press, so it cannot pick or drag. Opening, creating, or closing a project, and
  New or Open for a scene, stop the session first.
- **Errors.** Script and physics failures are logged to the Errors panel as they happen.

Animation machines are advanced by Play once a script or the host adds them to `PlaySession::animators()`; they step on each fixed
step, after the scripts. GPU particles are not advanced by Play. Entity-bound animation and particles are not created from the scene,
because this build's editor component catalog has no component that binds them to an entity.


- The viewport draws the scene's entity markers and a ground grid, not the rendered meshes. The scene render target is not yet shown
  inside the panel.
- A drag through a parent whose transform cannot be represented exactly (a shear, or a zero scale) is refused with a console note.
- Rotation with a non-uniformly scaled, rotated parent is approximate.
- Entities inside a prefab instance are not in the hierarchy, so they cannot be picked.
- The Hierarchy and Inspector panels are always shown; they are not in the View menu. They are added to the default layout only.
- The font is rasterized at the DPI of the window at startup. A later DPI change keeps the old size.
- The frame waits for its own fence before the next frame, so the UI renders one frame at a time. This is simple and correct, and it
  is not a performance target yet.
- There is no native file dialog. Folders are typed into the Project panel.
- Play does not render the play world; the viewport still draws the edited scene's markers during Play.
- Entity-bound physics bodies, scripts, animation, and particles are not created from the scene: the editor catalog has no such
  components yet, so the play world's systems run without content.
- The play world's audio is offline (no device), so Play produces no sound in the editor.

## Asset browser (US-066)

The Assets panel (left column, with Project and Hierarchy) browses the open project. `AssetBrowser` (`asset_browser.hpp`) has no ImGui or
graphics dependency: it presents the project's scan, runs the import, and carries out the rename and delete. `EditorModel::assets()` owns it.

- **Catalog and search.** Every source is listed: registered assets with their importer, and sources with no sidecar (`[no metadata]`),
  which have no GUID and so cannot be referenced. Search matches the path, the importer, or the GUID text, ignoring case.
- **Previews.** A texture shows a thumbnail decoded from its source (`converter::make_texture_preview`, the texture importer's decoder)
  and drawn as coloured cells. Other importers say why there is no preview. A preview is made again when the source's content hash changes.
- **Import settings.** The JSON object of importer settings is checked by the importer before the sidecar is written, so a rejected value
  leaves the sidecar as it was. The GUID, importer, and dependencies are kept by the rewrite.
- **Import.** Import cooks the project on a worker thread into `<project>/cooked` (the converter's Cook command). The progress bar counts
  finished assets. Cancel stops the run before the next asset starts; an asset that has started finishes and publishes, so no file is half
  written. A cancelled run says how many assets were not converted. Each diagnostic is logged with its code and kept in the Assets panel.
- **Hot reload.** A directory watch on the assets folder rescans the project after a change (debounced like the converter's watch). The
  selection is keyed by GUID, so it stays on the same asset, and only changed sources reload their thumbnails.
- **Drag and drop.** Dragging a registered asset onto an inspector Guid field sets that reference. Dropping it on the viewport places a new
  entity under the root: the first catalog component with a Guid field named after the importer (for example `texture`) gets the GUID.
  The drop goes through the scene's checked field edit. Without such a component the drop is refused and the reason is logged.
- **Rename and delete.** Both list the affected references before they run. An asset that depends on the asset is listed, and so is any
  json or text source that names its GUID. A rename keeps the GUID, so those references stay valid. A delete leaves them naming a GUID that
  no longer exists. Delete removes the source and its sidecar; the cooked output is left in place.

Known limits:

- The panel has no image of a model or a material; those importers have no preview yet.
- A drop is not checked against the field's kind beyond "a registered asset". The field's name is what the viewport drop uses.
- The reference search reads json and text sources only; a GUID inside a binary file is not found.
- Rename and delete do not rewrite references. GUIDs make that unnecessary, but a path written into text by hand is not updated.
- Cooked output is not removed on delete, and a cancelled import leaves the assets it finished converted.
- The watch does not run on a platform without ReadDirectoryChangesW or inotify; the browser then logs once and needs a reopen to refresh.

## Animation graph (US-070)

View > Animation Graph opens the animation graph window over `AnimationGraphDocument` (see `animation.md`). It creates, renames, and
removes states, sets each state's clip (only the clips that fit the skeleton are offered), speed, and default, adds and removes
parameters, and adds transitions with their timing, priority, and conditions. Refused edits explain why in the status line. The preview
runs the edited graph. The Play monitor reads a named machine from the live Play session. Content is the built-in demo rig, because there
is no clip importer yet. Undo and redo and save and open are in the document, so the window's buttons use the same history.

## Tests

- `engine_tests --suite editor` runs the CPU editor tests (`[editor]`): project and workspace files, the console, the model, the scene
  editor (`editor_scene_tests.cpp`), the viewport (`editor_viewport_tests.cpp`), the asset browser (`editor_asset_tests.cpp`), the input adapter,
  and a shell frame built on a headless ImGui context. These need no device.
- `engine_tests --suite editor --backend dx12|vulkan` adds the GPU test that draws rectangles, a clip rectangle, and Cyrillic text
  through the RHI and reads the pixels back. A GPU run is a report only on real hardware.
- A visual check of the running window is not part of the tests. Record screenshots from a native run and say so in the report.

## Input, focus, and shortcuts

- **Mouse buttons and the wheel come from window messages** (`WM_*BUTTON*`, `WM_MOUSEWHEEL`) with mouse capture while a button is held.
  Raw Input carries relative motion only. Raw Input follows keyboard focus, not the window under the cursor, so a click that activates the
  editor was lost and a click on a window overlapping the editor reached it.
- **Modifiers.** The adapter sends `ImGuiMod_Ctrl`, `Shift`, `Alt`, and `Super` as well as the left and right keys. Dear ImGui reads
  `io.KeyCtrl` from the `ImGuiMod_*` keys, so without them Ctrl+Z, Ctrl+A, and modifier clicks never saw their modifier.
- **Pointer.** It is read before the frame's button events, so a click lands where the cursor is now. While a button is held it keeps
  reporting outside the window, so a slider, a splitter, or a gizmo handle follows a drag that leaves the window.
- **Focus.** Losing focus releases every held key and button. An unfocused editor draws at a reduced rate.

| Shortcut | Action |
|---|---|
| Ctrl+Z, Ctrl+Y (Ctrl+Shift+Z) | Undo, redo |
| Ctrl+S | Save scene |
| Ctrl+D | Duplicate the selected entity |
| Delete | Delete the selected entity |
| W, E, R | Move, rotate, scale gizmo |
| F5 | Play, or stop when playing |
| F | Frame the selection (over the viewport) |
| RMB drag, RMB + WASD/QE | Orbit, fly |

Shortcuts do nothing while a text field has the keyboard, while Play runs (except F5), or with Ctrl/Alt held (except the Ctrl ones above).

## Components

The editor registers the built-in components (`docs/components.md`), so Add Component lists them from the first launch. The scene editor
loads every `.prefab` asset of the open project into its prefab library, so scenes with prefab instances open. Play creates physics bodies
and attaches Lua scripts from the scene's components, reading script sources from the project's `assets/` folder.

The sample project is `samples/showcase` (see its README).

## Opening scenes, the hierarchy, and the UI test

- **File > Open scene** lists the project's scenes. A double click on a scene in the Assets panel opens it. Both ask about unsaved changes.
- Hierarchy rows say what an entity is (its components, then its GUID prefix), because entities have no name field yet.
- `game_editor --log <file>` appends every console line to a file.
- **`tools/ui_tests/editor_ui_test.ps1`** drives the real window with real mouse and keyboard input (typing a path, a double click, Ctrl+D,
  Ctrl+Z, Ctrl+Y, Delete, Ctrl+S, F5) on a copy of the showcase, checks the editor's log, and saves screenshots to `artifacts/ui/`.
  It needs an unlocked Windows desktop. Its first run found that typed text never arrived: the message pump did not call `TranslateMessage`.
