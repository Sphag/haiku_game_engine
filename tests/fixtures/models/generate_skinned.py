#!/usr/bin/env python3
"""Writes skinned_arm.gltf: a two-joint arm with two clips, for the model importer's skeleton and animation tests.

A strip of three vertex rows (y = 0, 1, 2) is skinned to two joints: arm_root at the origin and arm_tip one meter above it. The bottom
row follows arm_root, the top row follows arm_tip, and the middle row is half and half. Two clips: "wave" turns arm_tip about Z over one
second, and "slide" moves arm_root along X over two seconds.

    python tests/fixtures/models/generate_skinned.py
"""
import base64
import json
import math
import struct
from pathlib import Path

positions = [(-0.5, 0, 0), (0.5, 0, 0), (-0.5, 1, 0), (0.5, 1, 0), (-0.5, 2, 0), (0.5, 2, 0)]
normals = [(0, 0, 1)] * 6
joints = [(0, 0, 0, 0), (0, 0, 0, 0), (0, 1, 0, 0), (0, 1, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
weights = [(1, 0, 0, 0), (1, 0, 0, 0), (0.5, 0.5, 0, 0), (0.5, 0.5, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
joints = [(0, 0, 0, 0), (0, 0, 0, 0), (0, 1, 0, 0), (0, 1, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
indices = [0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4]
inverse_bind = [
    (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),    # arm_root: identity
    (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1),   # arm_tip: moves the model down one meter into the joint's space
]


def quat_z(angle):
    return (0.0, 0.0, math.sin(angle / 2), math.cos(angle / 2))


wave_times = [0.0, 0.25, 0.5, 1.0]
wave_rotations = [quat_z(0.0), quat_z(0.6), quat_z(-0.6), quat_z(0.0)]
slide_times = [0.0, 2.0]
slide_translations = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0)]

blob = bytearray()
views = []
accessors = []


def add(data, component, count, kind, target=None, minmax=None):
    while len(blob) % 4:
        blob.append(0)
    views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), **({"target": target} if target else {})})
    blob.extend(data)
    accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count, "type": kind}
    if minmax:
        accessor["min"], accessor["max"] = minmax
    accessors.append(accessor)
    return len(accessors) - 1


FLOAT, USHORT, UINT = 5126, 5123, 5125
pos = add(b"".join(struct.pack("<3f", *p) for p in positions), FLOAT, 6, "VEC3", 34962,
          ([0, 0, 0], [0.5 * 2, 2, 0]) if False else ([-0.5, 0, 0], [0.5, 2, 0]))
nor = add(b"".join(struct.pack("<3f", *n) for n in normals), FLOAT, 6, "VEC3", 34962)
jnt = add(b"".join(struct.pack("<4H", *j) for j in joints), USHORT, 6, "VEC4", 34962)
wgt = add(b"".join(struct.pack("<4f", *w) for w in weights), FLOAT, 6, "VEC4", 34962)
idx = add(b"".join(struct.pack("<H", i) for i in indices), USHORT, len(indices), "SCALAR", 34963)
ibm = add(b"".join(struct.pack("<16f", *m) for m in inverse_bind), FLOAT, 2, "MAT4")
t_wave = add(b"".join(struct.pack("<f", t) for t in wave_times), FLOAT, 4, "SCALAR", None, ([0.0], [1.0]))
r_wave = add(b"".join(struct.pack("<4f", *q) for q in wave_rotations), FLOAT, 4, "VEC4")
t_slide = add(b"".join(struct.pack("<f", t) for t in slide_times), FLOAT, 2, "SCALAR", None, ([0.0], [2.0]))
p_slide = add(b"".join(struct.pack("<3f", *p) for p in slide_translations), FLOAT, 2, "VEC3")

document = {
    "asset": {"version": "2.0", "generator": "generate_skinned.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 2]}],
    "nodes": [
        {"name": "arm_root", "children": [1]},
        {"name": "arm_tip", "translation": [0, 1, 0]},
        {"name": "arm_mesh", "mesh": 0, "skin": 0},
    ],
    "meshes": [{"name": "arm", "primitives": [{"attributes": {"POSITION": pos, "NORMAL": nor, "JOINTS_0": jnt, "WEIGHTS_0": wgt},
                                                "indices": idx}]}],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": ibm, "skeleton": 0}],
    "animations": [
        {"name": "wave", "samplers": [{"input": t_wave, "output": r_wave, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]},
        {"name": "slide", "samplers": [{"input": t_slide, "output": p_slide, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]},
    ],
    "accessors": accessors,
    "bufferViews": views,
    "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode("ascii")}],
}
out = Path(__file__).with_name("skinned_arm.gltf")
with open(out, "w", encoding="utf-8", newline="\n") as handle:
    handle.write(json.dumps(document, indent=2) + "\n")
print("wrote", out)
