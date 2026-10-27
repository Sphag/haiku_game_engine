# Animation Playback and Skinning (US-036)

Skeletons, clips, pose sampling, two-clip blending, root motion, and skinning palettes live in `engine/animation/` (`engine_animation`). The library is CPU-only and does not depend on the renderer. The renderer takes plain matrices, the skinning palettes, and consumes them in `engine/render/`. The GPU skinning path and the motion vectors are in `engine/render/` (`scene_renderer.cpp`, `shaders/scene.hlsl`, `frame_snapshot.cpp`, and `reference_raster.cpp`).

## Conventions

- Translations are meters and rotations are unit quaternions `(w, x, y, z)`. Matrices are column-major, Y up, right-handed (see `runtime-conventions.md`).
- A skeleton lists joints parent-before-child. Each joint has a bind pose (local TRS) and an inverse bind matrix. `validate_skeleton` checks that the inverse bind undoes the bind pose.
- A pose is one local `JointPose` per joint. Its skinning palette is `model[j] * inverse_bind[j]`, which a vertex is skinned by.

## Sampling and looping

- A channel with no keys holds its bind-pose value. Translations and scales interpolate linearly, and rotations by slerp on the shorter arc.
- A looping clip is periodic. Between its last key and its first key the value wraps through the loop seam, so the pose is continuous at the seam. A non-looping clip holds its end keys and clamps its time.
- `ClipPlayer` advances with a speed (negative plays backward) and keeps an unwrapped `elapsed`. `cycles()` counts whole loops, and `finished()` is true once a non-looping clip ends.
- `sample_blend` mixes two clips on the same skeleton by a weight. The root displacement blends the same way.

## Root motion

`RootMotion` names the root joint and the axes to extract (X, Y, Z translation, and yaw). Extraction moves the displacement out of the pose and into the entity:

- `root_displacement(u)` is the entity's displacement from the clip start at unwrapped time `u`. Over whole cycles it adds the root's per-cycle motion (its last key minus its first), so a walk moves the entity forward on each loop.
- `remove_root_motion` holds the root's extracted translation at its start value and removes its yaw from the root rotation.

The entity's transform times the pose rebuilds the authored root. On the extracted components the root track is read non-periodically, so it is cumulative and does not blend across the seam. The root's other components keep their periodic values. `animation_tests.cpp` checks this reconstruction and the seam continuity.

## Animated bounds

`animated_bounds` returns the sphere that contains every skinned vertex at the clip's sampled poses. The samples are the clip ends, every key time, and the midpoints between consecutive key times. The radius is padded by 5 percent. Between samples the bound is not proven, but the midpoints and the padding cover the motion the tests exercise. The renderer culls and casts shadows with the sphere it is given (`SceneRenderer::set_mesh_bounds`).

## GPU skinning

A mesh with `joints` and `weights` uses a 52-byte vertex: position, normal, UV (locations 0 to 2), a packed joint word (location 3, one byte per influence), and four weights (location 4). Skinned items draw with five pipelines per mesh class (opaque, two-sided, blended, two-sided blended, and shadow). They use `VSSkinned` and `VSSkinnedShadow`, which blend the palette by the weights and then share the static path.

The palettes travel in the frame buffer after the item records, as the current pose's matrices and then the previous pose's matrices. A record holds the bone count, the palette's byte offset, and the previous model matrix. A skinned item with no pose draws the bind pose (an identity palette). The reference rasterizer skins the same way.

Normals are blended as a weighted sum of the palettes' upper 3x3 and then transformed by the item's normal matrix. This is exact for rigid joints and uniform scale. It is an approximation under non-uniform joint scale.

## Motion vectors

Each opaque pixel writes a motion vector to the `R32G32Float` velocity target. The value is the change in the point's screen position since the previous frame, in UV units (x right, y down). It is therefore the same on both backends. The previous clip position comes from:

- the previous view-projection in the camera record (byte 128), moved by the camera's motion, and
- the previous model matrix in the item record (byte 144), which includes the node's motion, and the previous palette for skinned vertices.

The first frame has no history, so its motion is zero. The renderer records the frame after a successful render. The scale is `(0.5, -0.5)` on DX12 and `(0.5, 0.5)` on Vulkan, because Vulkan's NDC y points down. Blended items write no motion vector, as with entity IDs.

## State machines (US-069)

A state graph is the versioned, data-only description of a character's states. It names clips and joints but does not hold them. `engine/animation/state_graph.hpp` parses and validates it, and `state_machine.hpp` runs it against a skeleton and a clip library. The Lua module `animation` (in `engine_script`) sets its parameters.

### Format

The JSON form has the format `engine.animation_graph` and version `1`. A reader refuses any other version by name. Keys are required unless noted:

```json
{
  "format": "engine.animation_graph", "version": 1,
  "parameters": [{"name": "speed", "type": "float", "default": 0}, {"name": "grounded", "type": "bool", "default": true}, {"name": "jump", "type": "trigger"}],
  "default_state": "idle",
  "root": {"joint": "root", "translate_x": true, "translate_y": false, "translate_z": true, "yaw": true},
  "states": [{"name": "idle", "clip": "idle", "speed": 1}, {"name": "walk", "clip": "walk", "speed": 1}],
  "transitions": [
    {"from": "idle", "to": "walk", "duration": 0.25, "priority": 1, "conditions": [{"when": "float", "parameter": "speed", "op": ">", "value": 0.1}]},
    {"from": "walk", "to": "idle", "duration": 0.25, "priority": 1, "conditions": [{"when": "float", "parameter": "speed", "op": "<=", "value": 0.1}]}
  ]
}
```

- `root` is optional. Without it the graph has no root motion.
- A condition is `{"when": "float", ...}` (with `op` one of `<`, `<=`, `>`, `>=`, `==`, `!=`), `{"when": "bool", "is": ...}`, `{"when": "trigger"}`, or `{"when": "finished"}`, which holds when the source state's non-looping clip has reached its end. A transition with no conditions always holds.
- `duration` is a blend in seconds and must be greater than zero. `priority` is required and must be distinct among the transitions that leave the same state.

### Rules

- **Entry.** A machine starts in `default_state` at time zero, and that entry is the first event it queues.
- **Priority.** The transitions leaving the active state are tried from highest priority to lowest. The first whose conditions all hold is taken. A tie is refused at validation, so the choice never depends on file order.
- **One transition at a time.** No transition is taken while a blend runs, so a blend always completes first.
- **Blend.** A transition's blend runs for its duration. The outgoing clip keeps playing during the blend, and the pose is the weighted blend of both clips, as `sample_blend` gives it.
- **Triggers.** A trigger stays set until a transition that tests it is taken. That transition clears it. A trigger set while no transition tests it stays set.
- **Update order.** One `update(dt)` advances the clips by `dt` times each state's speed, records the loop and finish events, completes a finished blend, takes the highest-priority satisfied transition (if no blend runs), and then samples the pose and the root delta.

### Events and root motion

- `take_events()` drains the queue of `Entered` (a state became active), `Loop` (the active clip crossed a cycle boundary, with the cycle it reached), and `Finished` (the active non-looping clip reached its end). Loop and finish events describe the active state's clip only.
- `root_delta()` is the change of the root's displacement in the update. The translation is in the root's own axes and the yaw is the heading change. The displacement is blended like the pose, so the deltas stay continuous through a blend. Their sum is the entity's motion since the start of the machine, and a state change that keeps the pose does not move the entity.

### Lua

The host binds an `AnimatorSet` through `ScriptServices::animators`, and each machine has a name. Scripts use:

| Call | Effect |
|---|---|
| `animation.set_float(name, param, value)` | Sets a float parameter. A non-finite value is refused. |
| `animation.set_bool(name, param, value)` | Sets a bool parameter. The value must be a Lua boolean. |
| `animation.trigger(name, param)` | Sets a trigger. |
| `animation.get_float`, `get_bool`, `get_trigger` | Read a parameter. |
| `animation.state(name)` | The active state's name. |
| `animation.blending(name)` | Whether a blend is running. |

A name that is not a machine, a parameter of another type, or a missing bound set raises a controlled Lua error, and nothing changes. A change reaches the machine immediately, but its transition is taken at the host's next `update`.

### Tests

`tests/src/animation_state_machine_tests.cpp` steps machines with a fixed 1/64 s, which every sum in them represents exactly. It checks the JSON round trip, the version and validation errors, entry, float/bool/trigger conditions, priority, trigger consumption, blend timing and pose, loop and finish events, root motion over a cycle, and continuity of root motion through a blend. `tests/src/script_animation_tests.cpp` checks the Lua calls, their errors, and the stack balance.

### Limits

- A blend cannot be interrupted, and a transition is not taken while one runs. Transitions from a blend's target wait until it completes.
- Events come only from the active state's clip. An outgoing clip that loops or finishes during a blend raises no event.
- No blend trees, sub-state machines, or transition offsets yet.
- Clips are still built in code. The graph names clips by name, so the caller supplies them.

## Graph editor (US-070)

`AnimationGraphDocument` (`graph_edit.hpp`, no ImGui or device) is the document the editor's Animation Graph window edits. It holds the state graph, the skeleton the clips are checked against, the clip library, and an undo/redo history.

- **Checked edits.** Every edit builds a candidate graph and runs it through `StateMachine::create`, the same check a runtime machine gets. A candidate the runtime would refuse is refused with that message and changes nothing, so the document is always a valid, runnable graph. Edits: add, remove, rename, clip, speed, and default state; add and remove parameters (a parameter a condition tests cannot be removed); add a transition (its priority is one above the source's highest); set a transition's duration and priority; add, set, and remove conditions; set the skeleton.
- **Compatibility.** A clip is valid for a skeleton when its tracks match the joints (`validate_clip`). `compatible_clips()` lists the library's valid clips for the picker. A clip or skeleton that breaks an existing state is refused, and the message names the state's clip.
- **Undo and redo.** Each applied edit is one entry. The history holds the graph and the skeleton together. The clip library is not edited, so it is not in the history. A refused edit adds nothing.
- **Save and open.** `save()` writes the state graph JSON (version 1), valid by construction. `open()` parses it and checks it against a skeleton and library, so a file that names a clip the skeleton does not fit is refused.
- **Preview.** `preview()` builds a `StateMachine` from the current graph. The window rebuilds it when the document's revision changes, so the preview always runs the edited graph.
- **Window.** View > Animation Graph. It lists parameters and states, edits the selected state's clip (only compatible clips are offered), speed, and default, lists transitions with their timing and conditions, and shows the preview (state, blend progress or timeline, parameter controls). Edits that are refused show their message in the status line.
- **Play monitor.** `PlaySession::animators()` is the session's named machines. The Lua `animation` module reaches them through `ScriptServices::animators`, and they advance on each fixed step after the scripts' `fixed_update`. The window's monitor reads a named machine from the live session, so a value a script sets shows there.

Limits:

- The window opens graphs against the built-in demo rig (a two-joint skeleton with idle, walk, and jump clips), because there is no clip importer yet. The skeleton picker lists only the rig.
- `open` takes its skeleton from the caller; a graph file does not name one.
- States and transitions are listed and edited from lists, not drawn on a canvas.
- Entity-bound animation is still not created from a scene, because the editor's component catalog has no animation component.
- A drag in a numeric field commits on Enter, not per frame, so one typed value is one undo entry.

## Tests

| Test | Checks |
|---|---|
| `tests/src/animation_graph_edit_tests.cpp` `[unit]` | Creation, state and transition and condition edits, refusals that change nothing, compatible clips and skeletons, undo and redo, save and reopen, and a preview that follows edited triggers. |
| `tests/src/animation_tests.cpp` `[unit]` | Skeleton and clip validation, the reference pose, interpolation, the loop seam, playback time, root motion reconstruction, blending, and animated bounds. |
| `tests/src/animation_state_machine_tests.cpp` `[unit]` | The state graph format and its validation, creation errors, entry, conditions, priority, triggers, blends, loop and finish events, and root motion through a blend. |
| `tests/src/script_animation_tests.cpp` `[unit]` | The Lua `animation` module: parameter changes, transitions after a host update, controlled errors, and the stack balance. |
| `tests/src/render_skin_tests.cpp` `[unit]` | Skin data validation, snapshot palettes and previous transforms, a rest pose reproducing the static mesh pixel for pixel, a moved palette matching a moved node (including motion vectors), and camera motion. |
| `tests/src/render_skin_gpu_tests.cpp` `[gpu][dx12]`, `[gpu][vulkan]` | A two-bone skinned box on the GPU, with a shadow-casting sun. Pick ids, depth, and motion vectors match the CPU reference, and the first frame has zero motion. |

## Scope and limits

- **Import is not in this story.** The Assimp adapter skips skins and animations (`IMPORT_UNSUPPORTED`, see `model-import.md`), and `tasks/prd.json` marks US-022 as passing without an importer for them. Clips and skeletons are built in code for now, so the fixtures are the clip definitions, not glTF files.
- Clip events are not implemented (US-022 lists them), and nothing consumes them.
- There is no blend tree. The state machine (US-069) is described above.
- The shadow and motion-vector paths follow the scene pass. A skinned item's shadow key includes its palette, so a moved caster redraws its atlas.
- The velocity target is a single-sample value. Temporal reprojection is US-039.
