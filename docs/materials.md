# Physically based materials and transparency (US-034)

US-034 adds physically based materials to the scene renderer (`engine_render`, `engine/render/`). A material sets metallic and roughness,
base color, emissive, and occlusion. It may name a texture for each of those and for a normal map. Materials support opaque, alpha-mask,
alpha-blend, and two-sided rendering, and they are lit by image-based lighting (IBL). Material instances override a parent's parameters.
The shading model is in `pbr.hpp`, and the GPU shader is `engine/render/shaders/scene.hlsl`. The shader mirrors the CPU reference term
for term, and the GPU tests compare the two on DX12 and Vulkan.

## Material model

`MaterialParams` (`material.hpp`) holds the factors: `base_color` (linear RGBA), `emissive` (linear RGB), `metallic`, `roughness`,
`occlusion_strength`, `normal_scale`, `alpha_cutoff`, the blend mode, and `two_sided`. `validate_material` rejects factors outside
their ranges and non-finite values.

`MaterialTextures` names one uploaded texture per slot. An unset slot uses a default: white for base color, metallic-roughness,
emissive, and occlusion, and a flat normal. The shading is

- **Base.** `albedo = srgb(base map) * base_color.rgb * tint.rgb`, where `tint` is the renderable's color.
- **Metallic-roughness.** `roughness = G * roughness`, and `metallic = B * metallic`, which is the glTF convention.
- **Dielectric and metal.** The Fresnel value at normal incidence is `F0 = mix(0.04, albedo, metallic)`.
- **Light.** Diffuse is `albedo * (1 - metallic) * (1 - F_spec) * E(N) / pi`. Specular is `prefiltered(R, roughness) * F_spec`.
  `F_spec = F0 * A + B` comes from the split-sum table at `(n . v, roughness)`. Occlusion scales both terms, and emissive is added.
- **Normal.** The tangent-space normal `(map * 2 - 1)`, with its x and y scaled by `normal_scale`, is rotated by the tangent frame.

## Color spaces

Base color and emissive maps hold sRGB bytes. The shader decodes them to linear before any math, and the output is encoded back to sRGB
for the 8-bit target. Normal, metallic-roughness, and occlusion maps are linear data. The factors are linear. `srgb_to_linear` and
`encode_srgb8` in `pbr.hpp` implement the IEC 61966-2-1 transfer function, and the tests round-trip every byte through both.

The output is clamped to `[0, 1]` before encoding, because tone mapping and HDR targets arrive with US-039.

## Image-based lighting

`build_environment` derives all of the IBL from one equirectangular radiance image:

- **Diffuse.** The image is projected onto nine SH bands and convolved with the cosine lobe. The coefficients are stored divided by pi,
  so a uniform environment of radiance `L` gives back `L` for a white diffuse surface. The coefficients are normalized to exactly `4 pi`.
- **Specular.** Level 0 is the source image. Levels 1 to 4 are GGX-prefiltered at roughness `i / 4`, with 256 Hammersley samples per texel
  and `N = V = R`, at a quarter of the previous size in each axis.
- **BRDF table.** A 32 x 32 table of `(A, B)` is indexed by `n . v` and roughness, and built with 256 importance samples per entry.

`SceneRenderer::set_environment` uploads the result. Each renderer starts with a uniform environment of radiance one, and a new call
replaces the environment on the next frame.

## Blend modes and the transparency policy

| Mode | Pipeline | Depth | Output |
|---|---|---|---|
| `Opaque` | opaque pass | test and write | alpha 1 |
| `AlphaMask` | opaque pass | test and write | discard below `alpha_cutoff`, otherwise alpha 1 |
| `AlphaBlend` | blended pass | test, no write | `src * a + dst * (1 - a)` |

A render draws the opaque pass first and the blended pass second, in the same render-graph pass:

1. **Opaque and alpha mask.** Items are grouped by mesh and material, and each group is one instanced draw. Order does not matter, because
   the depth test resolves it. Entity IDs and depth values are written here.
2. **Alpha blend.** Each item is its own draw, in this order:
   - Items are sorted **back to front** by the distance from the eye (the camera-relative origin) to the center of the mesh's bounding
     sphere, after the model matrix is applied.
   - Equal distances are broken by pick ID in ascending order, so the order is deterministic.
   - The blended pass does not write entity IDs or depth values, so picking returns the opaque surface behind a transparent one.

This is a sorted-by-center policy. It is exact for layers that do not overlap in depth. It is not order-independent transparency, so
intersecting or interpenetrating translucent meshes can blend in the wrong order. Order-independent transparency is not in this version.

Blending runs on the encoded 8-bit target. The shader encodes to sRGB before the blend, so the arithmetic is not linear light. A linear
HDR target arrives with US-039.

The blend uses `ONE` and `INV_SRC_ALPHA` for the destination alpha. Alpha is therefore coverage over the destination's alpha, and a
blended pixel over the cleared background keeps its own coverage.

## Two-sided and culling

Front faces are counter-clockwise as the viewer sees them on screen (the glTF convention). DX12 sets `FrontCounterClockwise`, and Vulkan
uses `VK_FRONT_FACE_COUNTER_CLOCKWISE`. The GPU suites check that both backends cull the same faces.

A one-sided material culls back faces. A two-sided material draws them, and on a back face the shading normal and the bitangent are
turned around, so the lit side is the one that faces the viewer. Each material's pipeline is chosen from its `two_sided` flag and its
blend mode, so there are four pipelines: opaque, opaque two-sided, blended, and blended two-sided.

## Material instances

`SceneRenderer::create_material_instance(parent, overrides)` creates a new material from the parent's parameters and textures, with the
set fields of `MaterialOverrides` applied. An unset field keeps the parent's value. The instance has its own parameter buffer and
descriptor set, so its values cannot change the parent's.

## Vertices and meshes

Vertices now carry a position, a normal, and a texture coordinate, at locations 0, 1, and 2. Meshes may omit the normals, and then
`vertex_normals` derives them from the winding, weighted by area. Missing UVs are zero. `make_box_mesh` has 24 vertices, with flat face
normals and 0 to 1 UVs on each face. `make_sphere_mesh` and `make_plane_mesh` are new.

The tangent frame for normal maps is computed in the shader from screen-space derivatives of the position and the UV. Vertices have no
tangent attribute yet.

## Shader resources

Each draw binds three descriptor sets (`scene.hlsl` has the register layout). The set index is the register space, and the binding is the
register number, on both backends.

- **Set 0**, the frame: the camera, one 208-byte record per item, and the palettes of skinned items (`docs/scene-rendering.md`).
- **Set 1**, the material: the parameters at `t0`, and five maps at `t1` to `t5`.
- **Set 2**, the environment: the SH coefficients at `t0`, five specular levels at `t1` to `t5`, and the BRDF table at `t6`.

Textures and the environment are read with `Load` at the nearest texel of `fract(uv)`, so no sampler is needed. The BRDF table is
bilinear between its four nearest entries. The GPU and the CPU reference therefore pick the same texels and the same weights.

## Verification

- **CPU** (`tests/src/render_material_tests.cpp`, `[unit]`): sRGB round trips, equirect mapping, the uniform-environment reproduction in
  diffuse and specular, the BRDF table bounds and the head-on limit, energy conservation of a white dielectric under a uniform light,
  metal and emissive extremes, normal-map tilt, the blend-mode output rules, material validation, instance overrides, radiance validation,
  and the orientation of generated meshes.
- **GPU** (`tests/src/render_material_gpu_tests.cpp`, `[gpu]`, on DX12 and Vulkan with the debug layers on): every pixel of each scene is
  compared with `shade_reference` evaluated on the surface that pixel shows, within two 8-bit steps. The scenes cover:
  - metallic and roughness at 0 and 1, on four quads;
  - sRGB base color with a gray texel, which would come out about 186 if read as linear;
  - alpha-mask edges on a checkerboard, including the discarded pixels' pick, color, and depth;
  - alpha-blend order over an opaque floor, with the submission order reversed;
  - one-sided culling and two-sided drawing of the same back face;
  - occlusion and emissive maps, with an exact expected color;
  - a normal map that tilts the lit surface;
  - material instances, and the parent's unchanged values;
  - an environment change between frames.
- **Scene regression** (`render_scene_gpu_tests.cpp`): the golden scene's object colors now come from the CPU reference, not from flat colors.

## Limits of this version

- No direct lights. Directional, point, and spot lights, and shadows, arrive with US-035.
- Textures are uncompressed 8-bit RGBA, uploaded as they are, with one mip level. Sampling is nearest-texel with repeat addressing. Mip
  chains, trilinear and anisotropic filtering, and BC compression are not in this version (`docs/texture-import.md` describes the
  converter's BC path, which these materials do not use yet).
- Materials are in memory only. A material file format and the material graph (US-067) come later.
- Environments are radiance images on the CPU, with no HDR file import.
- Transparent materials do not write entity IDs or depth. A transparent surface cannot be picked directly.
- The blend is on the encoded 8-bit target, and the output is clamped to `[0, 1]`.
- The reference rasterizer (`rasterize_reference`) still draws flat colors. It checks geometry, IDs, and depth, not shading.
