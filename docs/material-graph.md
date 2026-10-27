# Material graph (US-067, editing US-068)

A material graph is a versioned DAG of nodes that describes a surface. The engine turns it into HLSL, so a material author does not
write shader code. The library is `engine/material_graph` (`engine_material_graph`). Its CPU tests are in `material_graph_tests.cpp`
and its DXC tests in `material_graph_compile_tests.cpp`.

## Format

A graph is a JSON document with the schema `engine.material_graph` and version `1`. A reader refuses any other version by name.

```json
{
  "schema": "engine.material_graph",
  "version": 1,
  "name": "sample",
  "textures": [
    {"name": "albedo", "color_space": "srgb"},
    {"name": "normal", "color_space": "linear", "fallback": "flat_normal"}
  ],
  "nodes": [
    {"id": "uv", "kind": "uv"},
    {"id": "albedo_tex", "kind": "texture_sample", "name": "albedo", "inputs": {"uv": "uv"}},
    {"id": "tint", "kind": "parameter", "type": "vec4", "name": "tint", "value": [1, 1, 1, 1]},
    {"id": "tinted", "kind": "arithmetic", "op": "multiply", "inputs": {"a": "albedo_tex", "b": "tint"}},
    {"id": "normal_tex", "kind": "texture_sample", "name": "normal", "inputs": {"uv": "uv"}},
    {"id": "normal", "kind": "normal_map", "inputs": {"texture": "normal_tex"}},
    {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "normal": "normal"}}
  ]
}
```

| Kind | Fields | Pins | Output |
| --- | --- | --- | --- |
| `constant` | `type`, `value` | none | the literal, baked into the source |
| `parameter` | `type`, `name`, `value` (default) | none | the instance value, read from the parameter block |
| `uv` | none | none | vec2 |
| `texture_sample` | `name` (a declared texture) | `uv` (vec2, required) | vec4; sRGB decoded when the texture says so |
| `arithmetic` | `op`: add, subtract, multiply, divide, min, max | `a`, `b` (required) | the operand type |
| `normal_map` | none | `texture` (vec4, required), `scale` (float, default 1) | tangent-space vec3 |
| `pbr_output` | none | `base_color` (vec4), `metallic`, `roughness` (float), `emissive` (vec3), `occlusion`, `alpha` (float), `normal` (vec3) | the surface; exactly one per graph |

Textures are declared once: `color_space` is `srgb` or `linear`, and `fallback` (`white` by default, or `black` or `flat_normal`) is
what an unbound texture samples as. It matches the renderer's default for an empty slot.

Pin types: a float feeds any vector pin (splatted), and a vec3 feeds a vec4 pin with w = 1. Nothing else converts. Arithmetic needs
equal operand types or one float operand.

Every node field is checked against its kind, so a misplaced field is an error and not a silent default. The canonical form
(`graph_to_json`) sorts keys and is what the editor saves. `parse_graph(graph_to_json(g))` gives back `g`.

## Errors

Errors are node-specific. Each names the node id and its kind, and the pin where one applies:

```
node 'mul' (arithmetic): inputs 'a' (vec4) and 'b' (vec2) must match, or one must be a float
node 'tex' (texture_sample): input 'uv' expects vec2, got vec3
node 'loop_a' (arithmetic): input 'a' closes a cycle through node 'loop_b'
```

Graph-level errors have no node: `the graph needs exactly one pbr_output node, found 2`. The validator reports every error it finds,
not only the first. A graph with errors is not generated and never reaches the compiler.

Identifiers (node ids, parameter and texture names) must be identifiers, because codegen puts them in HLSL names. The name
`alpha_cutoff` is reserved for every material's alpha cutoff.

## Generated source

`generate_hlsl` returns one HLSL source for the whole graph. Its layout:

- **Parameter block.** `cbuffer MgParams` at set 1, binding 0. Each parameter takes one float4 row, so a value's offset is
  `16 * row + 4 * component`. Row 0 is `alpha_cutoff`, which is `MaterialParams::alpha_cutoff`. The rest follow in name order.
- **Textures.** `Texture2D<float4> t_<name>` at set 1, binding `1 + i`, where `i` is the texture's position in name order. Binding 0
  is the parameter block, so every binding number in set 1 is distinct.
- **Surface.** `MgSurface MgEvaluate(float2 uv)` runs the nodes in dependency order and writes the PBR output with the same clamps as
  the CPU reference.
- **Entry.** `PSGraph` takes `TEXCOORD0` and `SV_IsFrontFace`, and writes three targets: color with alpha, the encoded normal with
  roughness, and emissive with occlusion.

Texture reads follow `FetchTexture` in `scene.hlsl`: nearest texel, repeat addressing, and no sampler, because the RHI has none.

The CPU reference (`SurfaceEvaluator`) runs the same graph with the same conversions, fallbacks, and clamps. It is the reference the
GPU result is judged against.

## Variants

A variant is a blend mode (opaque, alpha mask, alpha blend) and a sidedness. Each is a build-time define: `MG_BLEND_MODE` (0, 1, 2)
and `MG_TWO_SIDED` (0 or 1). The alpha mask discards below `p_alpha_cutoff`, and two-sided flips the normal of a back face. There are
six variants, and `compile_material` builds each for both DXIL (DX12) and SPIR-V (Vulkan), which makes twelve entries per graph.

## Cache keys and instance parameters

The source is written to `<generated_dir>/<hash>.hlsl`, where `<hash>` is the SHA-256 of the source. The request key covers the path,
the entry point, the profile, the target, and the defines, and the cache environment covers the compiler and the backend. Two rules
follow from this:

- **A topology or constant change** changes the source, so the path and the key change. The old entries stay cached, and going back
  to the old graph hits them.
- **A parameter value change does not change the source.** Instance values reach the shader through the parameter block
  (`pack_parameters`), so an instance is a buffer write and not a rebuild. A change to a parameter's default also leaves the source
  unchanged, because defaults are not in the source either.

The rule for a constant is deliberate: a constant is baked, so if a value should change per instance, it is a parameter.

## Compile results

`compile_material` returns the generated layout, the source path, and one `VariantBinary` for each variant and target. Each binary
has its bytecode, its reflection, its cache key, and whether it came from the cache. Reflection is checked against the generated
layout: the parameter block's member offsets and size, and the set 1 bindings. A block the compiler dropped because no variant reads
it is not an error, because nothing in it is bound.

Compile failures are returned as `CompileFailure`, with the graph errors or the compiler diagnostics. `describe()` gives the text
form.

## Editing (US-068)

`GraphDocument` (`edit.hpp`) is the editable form. It holds the graph, the layout, the selection, and an undo history, and it has no
ImGui or device dependency. The editor's window (`engine/editor/src/material_graph_panel.cpp`, View > Material Graph) draws it.

- Edits are commands on the document: add, remove, link, unlink, set a value, type, name, or operation, declare a texture, and move
  nodes. Each applied edit is one undo entry; undo and redo restore the graph and the layout together. A refused edit (an unknown node,
  a pin the kind does not have, a self link, removing the output) changes nothing and names the reason.
- An edit that breaks a graph rule is kept, so the author can repair it. The errors name their nodes, and the canvas outlines them. The
  last version that analyzed clean is kept as `last_valid()`: the preview draws it, and `save_graph()` refuses a broken graph. The file
  on disk is therefore always a valid graph.
- The layout is a separate document, `engine.material_graph.layout` (`{"schema", "version", "nodes": {"<id>": [x, y]}}`), saved beside
  the graph as `<graph path>.layout.json`. A layout entry for a missing node is dropped; a node with no position goes on a grid. Moving a
  node changes neither the graph nor the generated source, so no cache key changes. The cooked runtime reads only the graph.
- Graph rules are checked on open as well (`parse_graph`), so an opened document is valid. Nodes may be listed in any order; a link to a
  node that comes later in the list is valid.
- The preview is a CPU sphere shaded from `SurfaceEvaluator` samples of the last valid graph, so parameter values show as the evaluator
  holds them. It is not a GPU mesh: normal maps and roughness are not shaded, and the scene renderer draws no graph materials yet.

## Not yet done

- The scene renderer does not draw graph materials yet. The generated `PSGraph` compiles, but no pipeline binds it, and the visual
  editor (US-068) is the first consumer.
- The GPU pass that compares the generated shader with the CPU reference is not written. The CPU reference is tested on its own.
- Graph layouts are in the separate layout document (US-068), not in the graph format.
- The visual editor's preview is a CPU sphere, not a GPU mesh, and the canvas has no zoom or box selection.
