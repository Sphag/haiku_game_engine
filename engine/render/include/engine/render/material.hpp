#pragma once

// Material description (US-034): the metallic/roughness parameters, the texture slots, and material instances, which are a parent's
// parameters with overrides. The GPU side (SceneRenderer) uploads these; the shading model is in pbr.hpp.
//
// Color spaces. Base color and emissive maps hold sRGB-encoded values and are decoded to linear before use. Normal, metallic-roughness, and
// occlusion maps hold linear data. Factors in MaterialParams are linear. The shader decodes sRGB itself, so the GPU and the CPU reference
// read the same bytes.

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {

// Opaque: written with depth and no blending. AlphaMask: as Opaque, but fragments below alpha_cutoff are discarded. AlphaBlend: blended over
// what is already drawn, sorted back to front, and written without depth (see docs/materials.md for the ordering policy).
enum class BlendMode : std::uint8_t {
    Opaque,
    AlphaMask,
    AlphaBlend,
};
[[nodiscard]] std::string_view to_string(BlendMode mode) noexcept;

struct MaterialParams {
    glm::vec4 base_color{1.0f};       // linear RGBA factor; multiplies the base color map
    glm::vec3 emissive{0.0f};         // linear factor; multiplies the emissive map
    float metallic = 0.0f;            // 0 dielectric, 1 metal; multiplies the metallic map's blue channel
    float roughness = 0.5f;           // 0 mirror, 1 rough; multiplies the roughness map's green channel
    float occlusion_strength = 1.0f;  // 0 ignores the occlusion map, 1 applies it fully
    float normal_scale = 1.0f;        // scales the x and y of the tangent-space normal
    float alpha_cutoff = 0.5f;        // AlphaMask only: fragments with alpha below this are discarded
    BlendMode blend = BlendMode::Opaque;
    bool two_sided = false;  // draws back faces too; the shading normal is turned to face the viewer
};

// Returns the first problem, or nothing when every parameter is finite and in range.
[[nodiscard]] std::optional<std::string> validate_material(const MaterialParams& params);

// Sparse overrides of a parent's parameters. An unset field keeps the parent's value.
struct MaterialOverrides {
    std::optional<glm::vec4> base_color;
    std::optional<glm::vec3> emissive;
    std::optional<float> metallic;
    std::optional<float> roughness;
    std::optional<float> occlusion_strength;
    std::optional<float> normal_scale;
    std::optional<float> alpha_cutoff;
    std::optional<BlendMode> blend;
    std::optional<bool> two_sided;
};

// The parameters of an instance: the parent's values with the overrides applied. The result is validated by the caller.
[[nodiscard]] MaterialParams resolve(const MaterialParams& parent, const MaterialOverrides& overrides);

// Texture slots of a material. Each names a texture the renderer uploaded (SceneRenderer::upload_texture). An unset slot uses the
// renderer's default texture for that slot: white for base color, metallic-roughness, emissive, and occlusion, and a flat normal. A white
// map leaves the factors as they are.
struct MaterialTextures {
    std::optional<std::uint32_t> base_color;          // sRGB RGBA: RGB is the base color, A multiplies the coverage
    std::optional<std::uint32_t> metallic_roughness;  // linear: G is roughness, B is metallic
    std::optional<std::uint32_t> normal;              // linear tangent-space normal, stored as n * 0.5 + 0.5
    std::optional<std::uint32_t> emissive;            // sRGB RGB
    std::optional<std::uint32_t> occlusion;           // linear: R is the ambient occlusion factor
};

// A CPU image for upload: 8-bit RGBA, row 0 at the top (v = 0), and width * height * 4 bytes.
struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] const std::uint8_t* texel(std::uint32_t x, std::uint32_t y) const noexcept {
        return pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

// Returns the first problem, or nothing when the sizes are non-zero and the byte count matches.
[[nodiscard]] std::optional<std::string> validate_image(const ImageRgba8& image);

}  // namespace engine::render
