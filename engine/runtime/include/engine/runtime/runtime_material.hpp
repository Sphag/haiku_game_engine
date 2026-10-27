#pragma once

// Materials and textures for the game, from assets alone.
//
// A material is an instance asset (engine.material, version 1): a material graph, the textures bound to the graph's texture names, and
// values for its parameters.
//
//   { "format": "engine.material", "version": 1, "graph": "<graph asset guid>",
//     "textures":   { "albedo": "<texture guid>", "normal": "<texture guid>" },
//     "parameters": { "tint": [1, 0.9, 0.8, 1], "roughness": [0.6] },
//     "blend": "opaque", "two_sided": false }
//
// The renderer's surface model is fixed (base color, metallic, roughness, normal, emissive, occlusion, each a factor and an optional map),
// so the graph is read for what it feeds to the PBR output: a texture sample is that pin's map, a parameter or constant is its factor, and
// a multiply combines them. Arithmetic that mixes a texture with anything but a multiply cannot be expressed by the fixed model, and is
// reported as a note and left out. Generating shader code from the graph at run time needs the shader compiler and stays out of the game.

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/gameplay/animation_binding.hpp"
#include "engine/render/material.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::runtime {

// The slots of render::MaterialTextures that a material can bind.
inline constexpr const char* kSlotBaseColor = "base_color";
inline constexpr const char* kSlotNormal = "normal";
inline constexpr const char* kSlotMetallicRoughness = "metallic_roughness";
inline constexpr const char* kSlotEmissive = "emissive";
inline constexpr const char* kSlotOcclusion = "occlusion";

struct MaterialRecipe {
    render::MaterialParams params;
    std::map<std::string, core::Guid> textures;  // slot name -> texture asset
    std::vector<std::string> notes;              // what the fixed surface model could not take from the graph
};

// Resolves a material instance. `reader` supplies the graph's JSON by GUID. A graph or instance that is malformed, or a texture name the
// graph does not declare, is an error with the reason.
[[nodiscard]] core::Result<MaterialRecipe, std::string> resolve_material(std::string_view instance_text, const gameplay::AssetReader& reader);

// Mip 0 of a cooked texture payload as RGBA8. BC7 and BC5 are decoded; a BC5 normal gets its Z rebuilt.
[[nodiscard]] core::Result<render::ImageRgba8, std::string> decode_texture_payload(std::string_view payload);

}  // namespace engine::runtime
