#!/usr/bin/env python3
# Writes the model import fixtures (US-020) into this directory. Run it with `python3 -I generate_fixtures.py`; the output is
# checked in, so the script is only needed when a fixture changes. Each fixture is one triangle with known geometry, so the
# tests can check positions, normals, UVs, and transforms against values worked out here.
#
# Geometry: the triangle has vertices (0,0,0), (1,0,0), (0,1,0) in its mesh space, normals +Z, and UVs (0,0), (1,0), (0,1).
#
# triangle_tree.gltf / .glb: glTF is Y-up and in meters. Node "Holder" translates by (10,0,0). Its child "Tri" translates
#   by (0,2,0) and scales by 2. World positions are (10,2,0), (12,2,0), (10,4,0). The mesh has an explicit TANGENT and a
#   COLOR_0 attribute, which the importer reports as not imported. The material is a PBR material.
# fbx_cm_yup.fbx: FBX with UnitScaleFactor 1 (centimeters), Y-up. The triangle is 100 units, and its model translates by
#   (1000,0,0). World positions are (10,0,0), (11,0,0), (10,1,0) meters.
# fbx_m_zup.fbx: FBX with UnitScaleFactor 100 (meters), Z-up, right-handed. The model translates by (0,0,2) meters. The
#   triangle lies on the ground plane, so Y-up world positions are (0,2,0), (1,2,0), (0,2,-1).
# triangle.obj: OBJ in meters with normals and UVs, and a material library that is not present, so the import reports it.
import base64
import json
import pathlib
import struct

HERE = pathlib.Path(__file__).resolve().parent

POSITIONS = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
NORMALS = [(0.0, 0.0, 1.0)] * 3
TANGENTS = [(1.0, 0.0, 0.0, 1.0)] * 3
UVS = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
COLORS = [(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0)]
INDICES = [0, 1, 2]


def gltf_buffer() -> bytes:
    data = b""
    data += b"".join(struct.pack("<3f", *p) for p in POSITIONS)   # offset 0, 36 bytes
    data += b"".join(struct.pack("<3f", *n) for n in NORMALS)     # offset 36, 36 bytes
    data += b"".join(struct.pack("<4f", *t) for t in TANGENTS)    # offset 72, 48 bytes
    data += b"".join(struct.pack("<2f", *uv) for uv in UVS)       # offset 120, 24 bytes
    data += b"".join(struct.pack("<4f", *c) for c in COLORS)      # offset 144, 48 bytes
    data += struct.pack("<3H", *INDICES)                          # offset 192, 6 bytes
    return data + b"\0" * (-len(data) % 4)                        # pad to 4 bytes: 200 total


def gltf_document(buffer_uri):
    buffer_entry = {"byteLength": 200}
    if buffer_uri is not None:
        buffer_entry["uri"] = buffer_uri
    return {
        "asset": {"version": "2.0", "generator": "generate_fixtures.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "Holder", "translation": [10.0, 0.0, 0.0], "children": [1]},
            {"name": "Tri", "translation": [0.0, 2.0, 0.0], "scale": [2.0, 2.0, 2.0], "mesh": 0},
        ],
        "meshes": [{
            "name": "Triangle",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2, "TEXCOORD_0": 3, "COLOR_0": 4},
                "indices": 5,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "Paint",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.8, 0.2, 0.1, 1.0],
                "metallicFactor": 0.25,
                "roughnessFactor": 0.5,
            },
            "doubleSided": True,
            "alphaMode": "OPAQUE",
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 0.0]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36},
            {"buffer": 0, "byteOffset": 72, "byteLength": 48},
            {"buffer": 0, "byteOffset": 120, "byteLength": 24},
            {"buffer": 0, "byteOffset": 144, "byteLength": 48},
            {"buffer": 0, "byteOffset": 192, "byteLength": 6},
        ],
        "buffers": [buffer_entry],
    }


def write_gltf():
    data = gltf_buffer()
    uri = "data:application/octet-stream;base64," + base64.b64encode(data).decode("ascii")
    text = json.dumps(gltf_document(uri), indent=2) + "\n"
    (HERE / "triangle_tree.gltf").write_text(text, encoding="utf-8")


def write_glb():
    json_bytes = json.dumps(gltf_document(None), separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    bin_bytes = gltf_buffer()
    chunks = struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes
    chunks += struct.pack("<I4s", len(bin_bytes), b"BIN\0") + bin_bytes
    header = struct.pack("<4sII", b"glTF", 2, 12 + len(chunks))
    (HERE / "triangle_tree.glb").write_bytes(header + chunks)


def fbx_document(unit_scale, up_axis, up_sign, front_axis, front_sign, translation):
    vertices = ", ".join(f"{x * unit_scale_units(unit_scale):g},{y * unit_scale_units(unit_scale):g},{z * unit_scale_units(unit_scale):g}"
                         for (x, y, z) in POSITIONS)
    return f"""; FBX 7.4.0 project file
FBXHeaderExtension:  {{
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}}
GlobalSettings:  {{
	Version: 1000
	Properties70:  {{
		P: "UpAxis", "int", "Integer", "",{up_axis}
		P: "UpAxisSign", "int", "Integer", "",{up_sign}
		P: "FrontAxis", "int", "Integer", "",{front_axis}
		P: "FrontAxisSign", "int", "Integer", "",{front_sign}
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",{unit_scale}
	}}
}}
Objects:  {{
	Geometry: 100, "Geometry::Triangle", "Mesh" {{
		Vertices: *9 {{
			a: {vertices}
		}}
		PolygonVertexIndex: *3 {{
			a: 0,1,-3
		}}
		GeometryVersion: 124
		LayerElementNormal: 0 {{
			Version: 101
			Name: ""
			MappingInformationType: "ByVertice"
			ReferenceInformationType: "Direct"
			Normals: *9 {{
				a: 0,0,1,0,0,1,0,0,1
			}}
		}}
		LayerElementUV: 0 {{
			Version: 101
			Name: "UVMap"
			MappingInformationType: "ByVertice"
			ReferenceInformationType: "Direct"
			UV: *6 {{
				a: 0,0,1,0,0,1
			}}
		}}
		Layer: 0 {{
			Version: 100
			LayerElement:  {{
				Type: "LayerElementNormal"
				TypedIndex: 0
			}}
			LayerElement:  {{
				Type: "LayerElementUV"
				TypedIndex: 0
			}}
		}}
	}}
	Model: 200, "Model::Tri", "Mesh" {{
		Version: 232
		Properties70:  {{
			P: "Lcl Translation", "Lcl Translation", "", "A",{translation}
		}}
		Shading: T
		Culling: "CullingOff"
	}}
	Material: 300, "Material::Paint", "" {{
		Version: 102
		ShadingModel: "phong"
		Properties70:  {{
			P: "DiffuseColor", "Color", "", "A",0.8,0.2,0.1
		}}
	}}
}}
Connections:  {{
	C: "OO",200,0
	C: "OO",100,200
	C: "OO",300,200
}}
"""


def unit_scale_units(unit_scale):
    # The vertices are written in the file's own units: a file with UnitScaleFactor 1 counts centimeters, so the
    # triangle's 1 meter is 100 of them. A file with UnitScaleFactor 100 counts meters.
    return 100.0 if unit_scale == 1 else 1.0


def write_fbx():
    cm_yup = fbx_document(unit_scale=1, up_axis=1, up_sign=1, front_axis=2, front_sign=1,
                          translation="1000,0,0")
    (HERE / "fbx_cm_yup.fbx").write_text(cm_yup, encoding="utf-8")
    # Z-up, right-handed: right = +X, up = +Z, forward = -Y. The file's UpAxis and FrontAxis select these.
    m_zup = fbx_document(unit_scale=100, up_axis=2, up_sign=1, front_axis=1, front_sign=-1,
                         translation="0,0,2")
    (HERE / "fbx_m_zup.fbx").write_text(m_zup, encoding="utf-8")


def write_obj():
    text = """# triangle with normals and UVs; the material library is deliberately absent
mtllib missing.mtl
o Triangle
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl Paint
f 1/1/1 2/2/1 3/3/1
"""
    (HERE / "triangle.obj").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    write_gltf()
    write_glb()
    write_fbx()
    write_obj()
