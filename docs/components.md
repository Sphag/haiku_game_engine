# Built-in components and GameWorld

`engine/gameplay` (`engine_gameplay`) holds the components every tool and the runtime share, and `GameWorld`, which turns them into running
systems. The editor registers the components when it starts (`SceneEditor`), and the runtime registers them before it loads a scene, so a scene
authored in the editor loads in the packaged game with no extra step. Game code can add its own components to the same catalog.

## Components

Type IDs are the strings a scene file uses. Colors are linear RGB 0..1, angles are degrees, lengths are meters. A Guid field names an asset.

| Type ID | Fields |
|---|---|
| `engine.name_tag` | `tag` |
| `engine.mesh_renderer` | `mesh`, `material`, `visible`, `cast_shadows`, `receive_shadows`, `tint` |
| `engine.camera` | `vertical_fov_degrees`, `near_plane`, `far_plane`, `primary` |
| `engine.light` | `kind` (0 directional, 1 point, 2 spot), `color`, `intensity`, `range`, `cone_angle_degrees`, `cast_shadows` |
| `engine.rigid_body` | `motion` (0 static, 1 kinematic, 2 dynamic), `shape` (0 box, 1 sphere, 2 capsule), `size`, `mass`, `friction`, `restitution`, `trigger` |
| `engine.character_controller` | `height`, `radius`, `move_speed`, `jump_speed` |
| `engine.script` | `script` (a Lua asset), `enabled` |
| `engine.audio_emitter` | `clip`, `volume`, `looping`, `spatial`, `play_on_start` |
| `engine.animator` | `graph`, `model`, `speed` |
| `engine.particle_emitter` | `rate`, `lifetime`, `speed`, `size`, `color`, `max_particles` |
| `engine.terrain` | `heightmap`, `material`, `size_meters`, `height_scale` |
| `engine.environment` | `sky_color`, `ambient_color`, `fog_density`, `gi_probes`, `ray_traced_shadows`, `ray_traced_reflections` |
| `engine.ui_canvas` | `layout` (a Lua asset), `visible` |

Every numeric field has a range, so the inspector and the loader refuse a value that would break a system.

## What acts on them today

| Component | Acted on by |
|---|---|
| `engine.rigid_body` | `GameWorld` creates a Jolt body (box, sphere, or capsule; static, kinematic, or dynamic; trigger) |
| `engine.script` | `GameWorld` attaches the Lua source, and the runner drives `create`, `start`, `fixed_update`, `update`, `destroy` |
| the rest | stored, edited, saved, packed, and loaded. Their systems (render, audio emitters, animator, particles, terrain, environment, UI) are in the engine as libraries and are not yet driven from the components |

## GameWorld

```cpp
auto world = gameplay::GameWorld::create(scene, read_script, services);   // bodies and scripts from the scene's components
world->advance(1.0 / 60.0);                                               // fixed steps, scripts, pose write-back, scene update
```

`bind_scene` and `sync_scene` are the two functions behind it. The editor's Play session calls them too, so a scene behaves the same under
Play and in the runtime. Dynamic bodies write their pose to a root entity; a script's transform changes are re-applied through the scene so
world matrices stay correct.

`game_runtime --frames <n>` runs a packed scene through `GameWorld` for n frames at 60 Hz (default 60) and reports bodies, scripts, and
fixed steps. It exits 1 if a script fails.
