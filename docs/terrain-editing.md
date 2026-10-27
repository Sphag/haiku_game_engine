# Terrain Sculpting and Painting

Terrain edits (US-071) live in `engine/world/include/engine/world/terrain_edit.hpp`, on the CPU. The terrain itself is the grid described in `terrain.hpp` (US-047).

## Brushes

| Brush | Effect per stamp at the centre |
|---|---|
| `Raise` / `Lower` | Adds or removes `kBrushHeightStep * strength * weight` metres (0.25 m at full strength). |
| `Smooth` | Moves each sample toward the 3 x 3 average of its neighbours before the stamp, by `strength * weight`. |
| `Flatten` | Moves each sample toward `target_height`, by `strength * weight`. |
| `Paint` | Moves one splat layer toward 255 by `strength * weight`; the other layers scale so the sample still sums to 255. |

`radius` is in metres. `falloff` is the fraction of the radius over which the weight eases from one to zero (`0` is a hard edge). `brush_weight` gives the weight at a normalised distance.

## Strokes and undo

`TerrainDocument::begin_stroke`, `stamp`, and `end_stroke` make one stroke. The document records each touched sample's value before the stroke at its first touch, and its value after the stroke at `end_stroke`. A stroke that changes nothing adds no command. `undo` and `redo` restore those values. A new edit after an undo drops the redo history.

`cancel_stroke` restores the samples a stroke touched and adds no command.

## Tile boundaries and invalidation

Tiles read one global sample grid, so a sample on a shared edge is one value for both tiles. Heights and normals agree across the edge by construction. Each stamp reports the tiles that own a changed sample (`take_dirty_tiles`, row-major and unique), and each tile has a revision (`tile_revision`) that undo and redo also advance. The dirty set grows by one sample in each direction around a change, because a sample's normal reads both neighbours: the tile across an edge reads a sample it does not own, so it is dirty too. A consumer rebuilds the LOD meshes, RT geometry, and collider of a dirty tile and no other.

The collider of a tile is rebuilt with `refresh_terrain_collider` (`engine/physics/collision_cells.hpp`). It replaces the shape of the body the caller already added, and the body keeps its id, pose, and cell. A tile outside the terrain is refused and its shape is kept.

## Editor session (`TerrainEditor`, `TerrainPanel`)

`TerrainEditor` (`engine/editor/include/engine/editor/terrain_editor.hpp`) is the headless session: a document, the brush settings, one lazily built LOD-0 mesh per tile, and one collider per tile. A tile's mesh is keyed by the document's tile revision, so an undo without a refresh still returns the reverted mesh. `refresh(physics)` drops the meshes of the dirty tiles and refreshes every body whose revision is behind, so a body that failed earlier is retried. Without a physics world the bodies are left stale and refreshed by the next call that has one. `ensure_colliders` creates one body per tile in a physics world.

The Terrain window (View > Terrain, `terrain_panel.cpp`) creates, opens, and saves a terrain, edits the brush, and undoes or redoes. The map is the brush surface: the left button starts a stroke, the brush is stamped under the pointer each frame, and release ends it as one command. Closing the window inside a stroke ends it. The window has no physics world, so it refreshes meshes only. Terrain is not drawn in the scene viewport yet.

## Files

`save_terrain` and `load_terrain` write and read the `.terrain` format: magic `OWTR`, version 1, the description, the sample counts, then heights as IEEE bit patterns and splat bytes, all little-endian. The file is written atomically. `parse_terrain` refuses a wrong size, a bad description, or a non-finite height. A reopened terrain cooks to the same blobs as the edit (`prepare_world`), which the converter tests check.

## Not covered

- The Terrain window draws a height and layer map, not the 3D terrain, and it has no physics world, so its edits do not move colliders.
- The renderer and ray tracing do not hold a per-tile terrain cache, so the editor's invalidated meshes are not uploaded to the GPU. The dirty tiles and revisions are the contract they will use.
- All tiles render at LOD 0 in the editor, so no neighbour is stitched there.
- Tests cover the CPU side only (`editor_terrain_tests.cpp`, `world_terrain_edit_tests.cpp`). No GUI or GPU check of the sculpted terrain was run.
