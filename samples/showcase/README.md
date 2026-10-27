# Showcase

The sample project. It is a real project folder: open it in the editor and everything in it works.

```
game_editor --project samples/showcase
```

In the editor: **File > Open** is the Project panel (paste the folder), the Assets panel lists `assets/`, double-click `scenes/main.scene`
to open it, press **Play** (or F5) to run it, **File > Build game...** to package it.

## What is in it

| Folder | Feature it shows |
|---|---|
| `assets/scenes/main.scene` | A scene that uses all 13 built-in components, a parent/child pair, and three prefab instances with overrides |
| `assets/scenes/credits.scene` | A second scene (scene choice for Build Game) with a camera and a UI canvas |
| `assets/prefabs/crate.prefab` | A prefab with a root and a child, a rigid body, and a script |
| `assets/models/` | glTF binary (`props.glb`) and OBJ (`marker.obj`) through the model importer |
| `assets/textures/` | Color, normal, and UI textures through the texture importer |
| `assets/materials/` | Material graphs (`engine.material_graph`) with textures, a tint parameter, a normal map, and roughness |
| `assets/animation/hero.animgraph.json` | An animation state graph: float, bool, and trigger parameters, priorities, a "finished" transition |
| `assets/scripts/` | Lua: entity lifecycle (`create`, `update`, `fixed_update`, `destroy`), state export and import, events, UI callbacks |
| `assets/audio/` | A WAV and an OGG clip |
| `assets/terrain/island.terrain` | A 2 x 2 tile terrain with four splat layers |
| `assets/gi/probes.gi.json` | A probe volume description that the `gi_probes` importer bakes |
| `world/` | A world partition (3 x 3 cells of 64 m, an always-loaded layer). Open it in the World window |

Every asset has a `.meta` sidecar with a fixed GUID, so the project opens without importing anything by hand.

## The scene

| Entity | Components |
|---|---|
| `environment` | environment (fog, probe volume), name tag |
| `sun`, `lamp`, `spot` | directional, point, and spot lights |
| `camera` | primary camera |
| `ground`, `platform`, `zone`, `ball` | mesh renderer and rigid body: static, kinematic (spinner script), trigger, and a bouncing dynamic sphere |
| `terrain` | terrain from the height map |
| `hero` (+ `hero_hand`) | character controller, script, animator, audio emitter, mesh renderer |
| `campfire` | particle emitter, light, looping audio emitter |
| `hud` | UI canvas bound to `scripts/hud.lua` |
| `crate_0..2` | prefab instances; each overrides the rigid body's mass |

## How it is tested

`tests/src/showcase_tests.cpp` runs the project through the real code: the asset scan, scene and prefab loading, a 3-second simulation of the
whole scene (the ball falls and settles, the scripts run without an error), the editor opening, playing and stopping it, the animation graph,
the material graphs, the terrain and audio readers, the world partition, and Build Game, which imports every asset the scene reaches, packs
them, and launches the packaged game from a clean folder. A final test names one asset per feature area, so a feature with no sample fails.

## Regenerating

The files are generated and checked in. To change the sample, edit `tools/samples/generate_showcase.py` and run it:

```
python tools/samples/generate_showcase.py
```

The output is deterministic (GUIDs come from a fixed namespace), so a rerun with no change to the script changes no file.
