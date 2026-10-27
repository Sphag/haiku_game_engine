# World Partition Editing

The world window (US-072) edits a partitioned world: its cells, the objects in them, and their residency. The headless model is `engine/editor/include/engine/editor/world_editor.hpp`; the window is `world_panel.hpp`. The partition format and grid rules are in `docs/large-world.md` and `engine/world/include/engine/world/partition.hpp`.

## Where the edits live

- The authored `World` is held in memory by `WorldEditor`. It is the only copy of the edits. Unloading a cell drops only its streamed copy, so an unloaded or suppressed cell keeps its objects and its dirty flag.
- `save()` writes the dirty cells and the always-loaded layer to the world directory, and writes `world.json` (the grid: origin and cell size, which no cell file holds). A cell is clean after its write succeeds.
- Residency is a `StreamingManager` (US-049) over a derived stream cache, `<directory>/.stream`. The cache holds the current contents of every cell. It is rebuilt from the World before the next residency update after an edit, so streaming never reads a stale cell. The cache is safe to delete; the next session rebuilds it.

## Moves and GUIDs

`move_object` keeps the object's GUID, name, and references. A move between cells dirties the cell it leaves and the cell it enters, so `save()` writes both. Nothing else changes, so references into the object still resolve.

## Residency controls

- **Load** clears a suppression and pins the cell: it stays resident outside the prefetch ring.
- **Pin** keeps a cell resident outside the prefetch ring. A pinned cell that is outside the active window is loaded, not activated.
- **Unload** suppresses the cell and clears its pin. A suppressed cell is never resident, even inside the active window. Suppression wins over a pin.
- **Go to cell** puts the editor camera at the cell's centre. The camera is a plain XZ position that drives residency. It is not the gameplay camera.

Overrides live in `StreamingManager::set_pinned` and `set_suppressed` (`streaming.hpp`). They apply at the next update and safe point, and a stream rebuild carries them over.

## Scripts

The world editor holds no script engine. Its streaming manager has no subscriber, so a residency change runs no gameplay code. Scripts run only in a Play session (`play_session.cpp`, US-065), which is separate.

## Overlay

The overlay shows the streaming budgets per update (IO, decode, GPU upload), the window radii, the residency counts and peaks (active, resident, CPU and GPU bytes, transient bytes), the stage counters, and the failure, cancellation, and teleport counts. The map colours each cell by its state, and marks pinned, unloaded-by-you, unsaved, and camera cells.

## Limits

- No undo for world edits yet. Terrain has its own undo (`docs/terrain-editing.md`).
- Opening a world loads every cell into memory. The streaming budget governs the runtime and the stream cache, not the authoring copy.
- The map shows at most 24 cells per side. A wider world is selected by typing a cell's coordinates instead.
- The viewport does not draw cells or the editor camera position yet; the world window is the only view of residency.
