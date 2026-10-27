#!/usr/bin/env python3
"""Generates samples/showcase, the sample project that uses every engine feature.

The output is deterministic: GUIDs come from a fixed namespace and file names, and nothing reads the clock. Run it again and
`git diff` shows nothing. The world partition folder is written by `showcase_world` (it needs the engine's own writer), so run
that tool after this script; `tools/samples/README.md` has the order.

    python tools/samples/generate_showcase.py [output_dir]
"""

import json
import math
import shutil
import struct
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "samples" / "showcase"
ASSETS = OUT / "assets"
FIXTURES = ROOT / "tests" / "fixtures"
NAMESPACE = uuid.UUID("6f1c2d7e-3b4a-4c5d-8e9f-0a1b2c3d4e5f")


def guid(name):
    """A stable version-4 style GUID for a name."""
    raw = bytearray(uuid.uuid5(NAMESPACE, name).bytes)
    raw[6] = (raw[6] & 0x0F) | 0x40
    raw[8] = (raw[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(raw)))


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.replace("\r\n", "\n").encode("utf-8"))


def write_json(path, value):
    write_text(path, json.dumps(value, indent=2, ensure_ascii=False) + "\n")


class Asset:
    def __init__(self, rel, importer, settings=None, deps=()):
        self.rel = rel
        self.guid = guid(rel)
        self.importer = importer
        self.settings = settings or {}
        self.deps = list(deps)


REGISTRY = {}


def asset(rel, importer, settings=None, deps=()):
    entry = Asset(rel, importer, settings, deps)
    REGISTRY[rel] = entry
    return entry


def write_sidecar(entry):
    write_json(ASSETS / (entry.rel + ".meta"), {
        "format": "engine.asset_meta",
        "version": 1,
        "guid": entry.guid,
        "importer": {"type": entry.importer, "settings": entry.settings},
        "dependencies": sorted(set(entry.deps)),
    })


# --- binary sources -------------------------------------------------------------------------------------------------------------

def png(width, height, pixel):
    """An 8-bit RGBA PNG. pixel(x, y) returns an (r, g, b, a) tuple."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(bytes(pixel(x, y)))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")


def write_png(rel, width, height, pixel, settings):
    path = ASSETS / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png(width, height, pixel))
    return asset(rel, "texture", settings)


def checker(x, y):
    on = ((x // 8) + (y // 8)) % 2 == 0
    return (200, 160, 110, 255) if on else (120, 85, 50, 255)


def flat_normal(x, y):
    # A gentle bump pattern in tangent space, encoded as 0..255.
    nx = 0.25 * math.sin(x / 4.0)
    ny = 0.25 * math.sin(y / 4.0)
    nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
    return (int((nx * 0.5 + 0.5) * 255), int((ny * 0.5 + 0.5) * 255), int((nz * 0.5 + 0.5) * 255), 255)


def grass(x, y):
    shade = 90 + ((x * 7 + y * 13) % 40)
    return (50, shade, 45, 255)


def ui_icon(x, y):
    inside = (x - 16) ** 2 + (y - 16) ** 2 < 14 * 14
    return (240, 200, 60, 255) if inside else (0, 0, 0, 0)


def terrain_height(x, z, size):
    """Island hills in meters for the sample at (x, z) on a size x size grid."""
    u = x / size - 0.5
    v = z / size - 0.5
    falloff = max(0.0, 1.0 - 4.0 * (u * u + v * v))
    return falloff * (6.0 + 3.0 * math.sin(x * 0.21) * math.cos(z * 0.17))


def write_terrain():
    """The engine's .terrain format (engine/world/src/terrain_edit.cpp): OWTR, version 1, header, heights, splat."""
    tiles, cells = 2, 64
    samples = tiles * cells + 1
    heights = [terrain_height(x, z, samples - 1) for z in range(samples) for x in range(samples)]
    out = bytearray(b"OWTR")
    out += struct.pack("<7I", 1, tiles, tiles, cells, struct.unpack("<I", struct.pack("<f", 1.0))[0], samples, samples)
    for h in heights:
        out += struct.pack("<f", h)
    for z in range(samples):
        for x in range(samples):
            h = heights[z * samples + x]
            rock = min(255, int(h * 30))
            out += bytes((max(0, 255 - rock), rock, 0, 0))
    path = ASSETS / "terrain" / "island.terrain"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return asset("terrain/island.terrain", "blob")


# --- text sources ---------------------------------------------------------------------------------------------------------------

CRATE_LUA = """-- Spins a crate slowly and records how long it has been running. A script is attached through the engine.script component.
local elapsed = 0

function create(entity)
  elapsed = 0
end

function update(entity, dt)
  elapsed = elapsed + dt
  local t = transform.get(entity)
  local angle = elapsed * 0.5
  transform.set(entity, { rotation = { math.cos(angle / 2), 0, math.sin(angle / 2), 0 } })
end

function export_state()
  return { elapsed = elapsed }
end

function import_state(state)
  elapsed = state.elapsed or 0
end
"""

PLAYER_LUA = """-- The hero: reads input, moves the entity, steers the animation graph, and tells the HUD when it jumps. A host that does not bind
-- input or animation (a headless run) leaves those calls failing, so each is wrapped and the script keeps moving.
local speed = 0
local walk_speed = 3.0

local function try(fn, ...)
  local ok, value = pcall(fn, ...)
  if ok then return value end
  return nil
end

function create(entity)
  events.on('hud.reset', function() speed = 0 end)
end

function fixed_update(entity, dt)
  local move = try(input.value, 'move_forward') or 0
  speed = move * walk_speed
  local t = transform.get(entity)
  transform.set(entity, { position = { t.position[1], t.position[2], t.position[3] - speed * dt } })
end

function update(entity, dt)
  try(animation.set_float, 'hero', 'speed', math.abs(speed))
  if try(input.pressed, 'jump') then
    try(animation.trigger, 'hero', 'jump')
    events.emit('hero.jump', entity)
  end
end

function destroy(entity)
  events.emit('hero.gone', entity)
end
"""

HUD_LUA = """-- Builds the heads-up display callbacks. The host binds a button to on_play with script.ui_action; the label text is plain data.
presses = 0
volume = 0.8

function on_play()
  presses = presses + 1
  events.emit('hud.play', presses)
end

function on_volume(value)
  volume = value
  audio.set_bus_volume('master', value)
end
"""

SPINNER_LUA = """-- Turns a kinematic platform around the vertical axis at a fixed rate in the fixed step.
local angle = 0

function fixed_update(entity, dt)
  angle = angle + dt
  transform.set(entity, { rotation = { math.cos(angle / 2), 0, math.sin(angle / 2), 0 } })
end
"""

HERO_GRAPH = {
    "format": "engine.animation_graph", "version": 1,
    "parameters": [
        {"name": "speed", "type": "float", "default": 0},
        {"name": "grounded", "type": "bool", "default": True},
        {"name": "jump", "type": "trigger"},
    ],
    "default_state": "idle",
    "root": {"joint": "root", "translate_x": True, "translate_y": False, "translate_z": True, "yaw": True},
    "states": [
        {"name": "idle", "clip": "idle", "speed": 1},
        {"name": "walk", "clip": "walk", "speed": 1},
        {"name": "jump", "clip": "jump", "speed": 1},
    ],
    "transitions": [
        {"from": "idle", "to": "walk", "duration": 0.25, "priority": 1,
         "conditions": [{"when": "float", "parameter": "speed", "op": ">", "value": 0.1}]},
        {"from": "walk", "to": "idle", "duration": 0.25, "priority": 1,
         "conditions": [{"when": "float", "parameter": "speed", "op": "<=", "value": 0.1}]},
        {"from": "idle", "to": "jump", "duration": 0.15, "priority": 5, "conditions": [{"when": "trigger", "parameter": "jump"}]},
        {"from": "walk", "to": "jump", "duration": 0.15, "priority": 5, "conditions": [{"when": "trigger", "parameter": "jump"}]},
        {"from": "jump", "to": "idle", "duration": 0.25, "priority": 1, "conditions": [{"when": "finished"}]},
    ],
}


def material_graph(name, texture_names, tint):
    nodes = [{"id": "uv", "kind": "uv"}]
    textures = []
    last = None
    for tex, space in texture_names:
        textures.append({"name": tex, "color_space": space} if space == "srgb" else {"name": tex, "color_space": space, "fallback": "flat_normal"})
    nodes += [
        {"id": "albedo_tex", "kind": "texture_sample", "name": "albedo", "inputs": {"uv": "uv"}},
        {"id": "tint", "kind": "parameter", "type": "vec4", "name": "tint", "value": tint},
        {"id": "tinted", "kind": "arithmetic", "op": "multiply", "inputs": {"a": "albedo_tex", "b": "tint"}},
        {"id": "normal_tex", "kind": "texture_sample", "name": "normal", "inputs": {"uv": "uv"}},
        {"id": "normal", "kind": "normal_map", "inputs": {"texture": "normal_tex"}},
        {"id": "rough", "kind": "parameter", "type": "float", "name": "roughness", "value": [0.7]},
        {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "normal": "normal", "roughness": "rough"}},
    ]
    return {"schema": "engine.material_graph", "version": 1, "name": name, "textures": textures, "nodes": nodes}


GI_SOURCE = {
    "grid": {"origin": [-8, 0.5, -8], "spacing": 4, "dims": [5, 2, 5]},
    "sky": {"zenith": [0.35, 0.55, 0.95], "horizon": [0.75, 0.8, 0.85], "ground": [0.12, 0.1, 0.08],
            "sun_direction": [0.3, 0.8, 0.2], "sun_radiance": [3.0, 2.8, 2.5]},
    "occluders": [{"min": [-1, 0, -1], "max": [1, 2, 1], "albedo": [0.6, 0.5, 0.4], "emitted": [0, 0, 0]}],
    "samples": 64,
    "max_ray_distance": 100,
}


# --- scene ----------------------------------------------------------------------------------------------------------------------

IDENTITY = {"position": [0, 0, 0], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1]}


def about_x(degrees):
    """A unit quaternion (w, x, y, z) for a rotation about the X axis."""
    half = math.radians(degrees) / 2.0
    return (math.cos(half), math.sin(half), 0.0, 0.0)


def xform(position=(0, 0, 0), rotation=(1, 0, 0, 0), scale=(1, 1, 1)):
    length = math.sqrt(sum(c * c for c in rotation))
    return {"position": list(position), "rotation": [c / length for c in rotation], "scale": list(scale)}


def comp(**fields):
    return {"schema": 1, "fields": fields}


def entity(name, transform, components=None, parent=None):
    return {"guid": guid("entity/" + name), "parent": parent, "transform": transform, "components": components or {}}


def build_scene(a, prefab):
    sun_rotation = about_x(-50)
    hero = guid("entity/hero")
    entities = [
        entity("environment", xform(), {
            "engine.name_tag": comp(tag=1),
            "engine.environment": comp(gi_probes=a["gi"].guid, fog_density=0.01, ray_traced_shadows=False),
        }),
        entity("sun", xform((0, 10, 0), sun_rotation), {
            "engine.light": comp(kind=0, intensity=3.0, color=[1.0, 0.95, 0.85], cast_shadows=True),
        }),
        entity("lamp", xform((3, 2.5, -2)), {
            "engine.light": comp(kind=1, intensity=40.0, range=12.0, color=[1.0, 0.7, 0.4]),
            "engine.mesh_renderer": comp(mesh=a["marker"].guid, material=a["crate_mat"].guid, cast_shadows=False),
        }),
        entity("spot", xform((-4, 4, 2), about_x(-45)), {
            "engine.light": comp(kind=2, intensity=80.0, range=15.0, cone_angle_degrees=40.0),
        }),
        entity("camera", xform((0, 4, 10), about_x(-10)), {
            "engine.camera": comp(vertical_fov_degrees=65.0, far_plane=500.0, primary=True),
        }),
        entity("ground", xform((0, -0.5, 0), scale=(20, 1, 20)), {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["ground_mat"].guid, receive_shadows=True),
            "engine.rigid_body": comp(motion=0, shape=0, size=[40.0, 1.0, 40.0], friction=0.8),
        }),
        entity("terrain", xform((-64, -6, -64)), {
            "engine.terrain": comp(heightmap=a["heightmap"].guid, material=a["ground_mat"].guid, size_meters=[128.0, 128.0], height_scale=1.0),
        }),
        entity("hero", xform((0, 1, 4)), {
            "engine.name_tag": comp(tag=2),
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["crate_mat"].guid),
            "engine.character_controller": comp(height=1.8, radius=0.35, move_speed=3.0, jump_speed=5.5),
            "engine.script": comp(script=a["player_lua"].guid),
            "engine.animator": comp(graph=a["hero_graph"].guid, model=a["props"].guid, speed=1.0),
            "engine.audio_emitter": comp(clip=a["tone"].guid, volume=0.6, looping=False, spatial=True, play_on_start=False),
        }),
        entity("hero_hand", xform((0.4, 1.0, 0)), {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["crate_mat"].guid, tint=[1.0, 0.8, 0.2, 1.0]),
        }, parent=hero),
        entity("campfire", xform((-3, 0.2, -1)), {
            "engine.particle_emitter": comp(rate=120.0, lifetime=1.5, speed=1.8, size=0.08, color=[1.0, 0.55, 0.2, 1.0], max_particles=4096),
            "engine.light": comp(kind=1, intensity=25.0, range=8.0, color=[1.0, 0.5, 0.2], cast_shadows=False),
            "engine.audio_emitter": comp(clip=a["ambience"].guid, volume=0.4, looping=True, spatial=True, play_on_start=True),
        }),
        entity("platform", xform((6, 0.5, 0), scale=(3, 0.3, 3)), {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["ground_mat"].guid),
            "engine.rigid_body": comp(motion=1, shape=0, size=[3.0, 0.3, 3.0]),
            "engine.script": comp(script=a["spinner_lua"].guid),
        }),
        entity("zone", xform((0, 1, -6)), {
            "engine.rigid_body": comp(motion=0, shape=1, size=[4.0, 4.0, 4.0], trigger=True),
        }),
        entity("ball", xform((2, 6, 2)), {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["crate_mat"].guid, tint=[0.2, 0.5, 1.0, 1.0]),
            "engine.rigid_body": comp(motion=2, shape=1, size=[0.8, 0.8, 0.8], mass=2.0, restitution=0.6),
        }),
        entity("hud", xform(), {
            "engine.ui_canvas": comp(layout=a["hud_lua"].guid, visible=True),
        }),
    ]
    for index, position in enumerate(((-2, 0.5, -3), (-2, 1.5, -3), (-1, 0.5, -3))):
        overrides = [{"entity": prefab["root"], "component": "engine.rigid_body", "field": "mass", "value": 2.0 + index}]
        entities.append({
            "guid": guid("entity/crate_%d" % index), "parent": None, "transform": xform(position),
            "prefab": {"guid": a["crate_prefab"].guid, "overrides": overrides},
        })
    return {"format": "engine.scene", "version": 1, "entities": entities}


def referenced_guids(value, known):
    """Every known asset GUID that appears as a string anywhere in a JSON value."""
    found = set()
    if isinstance(value, dict):
        for item in value.values():
            found |= referenced_guids(item, known)
    elif isinstance(value, list):
        for item in value:
            found |= referenced_guids(item, known)
    elif isinstance(value, str) and value in known:
        found.add(value)
    return found


def write_world(a):
    """A partitioned world beside the project (not inside assets): a 3 x 3 grid of 64 m cells with scattered props and an always-loaded
    layer. The formats are in engine/world/src/partition.cpp and engine/editor/src/world_editor.cpp."""
    out = OUT / "world"
    cell_size = 64
    origin = -96
    write_json(out / "world.json", {"format": "engine.world_desc", "version": 1, "origin": [origin, origin], "cell_size": cell_size})
    prop = a["props"].guid
    marker = a["marker"].guid
    layer_id = guid("world/layer/sun")
    write_text(out / "layer.world", "engine.world_layer 1\nobject %s %s 0 0 0 Sun marker\nend\n" % (layer_id, marker))
    for cz in range(3):
        for cx in range(3):
            coord = (cx, cz)
            cell_id = guid("world/cell/%d/%d" % coord)
            lines = ["engine.world_cell 1", "cell %s %d %d" % (cell_id, coord[0], coord[1]), "asset %s" % prop]
            base_x = origin + coord[0] * cell_size
            base_z = origin + coord[1] * cell_size
            for index in range(3):
                oid = guid("world/object/%d/%d/%d" % (coord + (index,)))
                x = base_x + 10 + index * 18
                z = base_z + 12 + ((index * 7) % 5) * 9
                lines.append("object %s %s %d 0 %d Tree %d.%d" % (oid, prop, x, z, cx * 3 + cz, index))
            lines.append("end")
            write_text(out / ("%s.cell" % cell_id), "\n".join(lines) + "\n")


# --- main -----------------------------------------------------------------------------------------------------------------------

def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    ASSETS.mkdir(parents=True)

    a = {}
    a["checker"] = write_png("textures/checker.png", 64, 64, checker, {"usage": "color"})
    a["normal"] = write_png("textures/bump_normal.png", 64, 64, flat_normal, {"usage": "normal"})
    a["grass"] = write_png("textures/grass.png", 64, 64, grass, {"usage": "color"})
    a["icon"] = write_png("textures/coin_icon.png", 32, 32, ui_icon, {"usage": "color"})
    a["heightmap"] = write_terrain()

    (ASSETS / "models").mkdir(parents=True)
    shutil.copyfile(FIXTURES / "models" / "triangle_tree.glb", ASSETS / "models" / "props.glb")
    a["props"] = asset("models/props.glb", "model", {"scale": 1.0})
    shutil.copyfile(FIXTURES / "models" / "triangle.obj", ASSETS / "models" / "marker.obj")
    a["marker"] = asset("models/marker.obj", "model", {"scale": 2.0})

    (ASSETS / "audio").mkdir(parents=True)
    shutil.copyfile(FIXTURES / "audio" / "tone_mono_s16.wav", ASSETS / "audio" / "tone.wav")
    a["tone"] = asset("audio/tone.wav", "blob")
    shutil.copyfile(FIXTURES / "audio" / "tone_stereo.ogg", ASSETS / "audio" / "ambience.ogg")
    a["ambience"] = asset("audio/ambience.ogg", "blob")

    write_text(ASSETS / "scripts" / "crate.lua", CRATE_LUA)
    a["crate_lua"] = asset("scripts/crate.lua", "text")
    write_text(ASSETS / "scripts" / "player.lua", PLAYER_LUA)
    a["player_lua"] = asset("scripts/player.lua", "text")
    write_text(ASSETS / "scripts" / "hud.lua", HUD_LUA)
    a["hud_lua"] = asset("scripts/hud.lua", "text", deps=[a["icon"].guid])  # the HUD draws the coin icon
    write_text(ASSETS / "scripts" / "spinner.lua", SPINNER_LUA)
    a["spinner_lua"] = asset("scripts/spinner.lua", "text")

    write_json(ASSETS / "animation" / "hero.animgraph.json", HERO_GRAPH)
    a["hero_graph"] = asset("animation/hero.animgraph.json", "json")

    write_json(ASSETS / "materials" / "crate.material.json", material_graph("crate", [("albedo", "srgb"), ("normal", "linear")], [1.0, 1.0, 1.0, 1.0]))
    a["crate_mat"] = asset("materials/crate.material.json", "json", deps=[a["checker"].guid, a["normal"].guid])
    write_json(ASSETS / "materials" / "ground.material.json", material_graph("ground", [("albedo", "srgb"), ("normal", "linear")], [0.8, 1.0, 0.8, 1.0]))
    a["ground_mat"] = asset("materials/ground.material.json", "json", deps=[a["grass"].guid, a["normal"].guid])

    write_json(ASSETS / "gi" / "probes.gi.json", GI_SOURCE)
    a["gi"] = asset("gi/probes.gi.json", "gi_probes")

    # The prefab: one root crate with a child. Entity GUIDs in a prefab are its local IDs.
    crate_root = guid("prefab/crate/root")
    prefab_doc = {"format": "engine.prefab", "version": 1, "entities": [
        {"guid": crate_root, "parent": None, "transform": xform(), "components": {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["crate_mat"].guid),
            "engine.rigid_body": comp(motion=2, shape=0, size=[1.0, 1.0, 1.0], mass=1.0, friction=0.6),
            "engine.script": comp(script=a["crate_lua"].guid),
        }},
        {"guid": guid("prefab/crate/lid"), "parent": crate_root, "transform": xform((0, 0.55, 0), scale=(1.0, 0.1, 1.0)), "components": {
            "engine.mesh_renderer": comp(mesh=a["props"].guid, material=a["crate_mat"].guid, tint=[0.9, 0.9, 0.6, 1.0]),
        }},
    ]}
    write_json(ASSETS / "prefabs" / "crate.prefab", prefab_doc)
    a["crate_prefab"] = asset("prefabs/crate.prefab", "json")

    scene_doc = build_scene(a, {"root": crate_root})
    write_json(ASSETS / "scenes" / "main.scene", scene_doc)
    known = {e.guid for e in REGISTRY.values()}
    deps = referenced_guids(scene_doc, known)
    asset("scenes/main.scene", "json", deps=deps)

    # A second, empty-ish scene so the start scene is a choice and scene switching has a target.
    credits = {"format": "engine.scene", "version": 1, "entities": [
        entity("credits_camera", xform((0, 1, 5)), {"engine.camera": comp()}),
        entity("credits_ui", xform(), {"engine.ui_canvas": comp(layout=a["hud_lua"].guid)}),
    ]}
    write_json(ASSETS / "scenes" / "credits.scene", credits)
    asset("scenes/credits.scene", "json", deps=[a["hud_lua"].guid])

    # Prefab and scene dependencies cover everything their entities name.
    REGISTRY["prefabs/crate.prefab"].deps = sorted(referenced_guids(prefab_doc, known))

    for entry in REGISTRY.values():
        write_sidecar(entry)

    write_world(a)
    write_json(OUT / "project.json", {"format": "engine.project", "version": 1, "name": "Showcase"})
    write_json(OUT / "start.json", {"start_scene": REGISTRY["scenes/main.scene"].guid, "backend": "dx12", "profile": "native"})
    print("wrote %d assets to %s" % (len(REGISTRY), OUT))
    print("start scene:", REGISTRY["scenes/main.scene"].guid)


if __name__ == "__main__":
    main()
