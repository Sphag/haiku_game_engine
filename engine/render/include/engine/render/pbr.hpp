#pragma once

// Physically based shading (US-034): color spaces, image-based lighting, and the CPU reference of the scene shader.
//
// The model is metallic/roughness with split-sum image-based lighting. The diffuse term is the irradiance of the environment, stored as
// nine spherical-harmonic coefficients. The specular term is a set of GGX-prefiltered equirectangular images, one per roughness step,
// scaled by the BRDF scale and bias from a precomputed table. Directional and local lights are not part of this model; they arrive with
// US-035. Output is linear radiance, clamped to [0, 1] and sRGB-encoded into the 8-bit target. Tone mapping and HDR targets arrive with
// US-039.
//
// Everything here runs on the CPU. shaders/scene.hlsl mirrors the same formulas, and the GPU tests compare the two, so the functions
// below are the golden source. The BRDF table is bilinear; the environment and material textures are nearest-texel with repeat addressing.
// Both sides use the same lookups, so they pick the same texels.

#include "engine/render/material.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

// Roughness steps of the prefiltered specular images: level i is GGX at roughness i / (kEnvironmentLevels - 1).
inline constexpr std::uint32_t kEnvironmentLevels = 5;
// Side of the square BRDF table, indexed by n . v (columns) and roughness (rows).
inline constexpr std::uint32_t kBrdfLutSize = 32;
// Hammersley samples per prefiltered texel and per table entry.
inline constexpr std::uint32_t kPrefilterSamples = 256;

// Linear radiance on an equirectangular map. Row 0 is the top (the +Y pole), and column 0 starts at direction +X (see
// direction_to_equirect).
struct EquirectImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<glm::vec3> texels;  // width * height, linear
};

// Returns the first problem, or nothing for a non-empty image with finite, non-negative radiance.
[[nodiscard]] std::optional<std::string> validate_equirect(const EquirectImage& image);

// The image-based lighting of one environment. All of it is derived from the radiance image by build_environment().
struct Environment {
    std::array<glm::vec3, 9> diffuse_sh{};  // irradiance / pi, as SH coefficients (see evaluate_diffuse)
    std::vector<EquirectImage> specular;    // kEnvironmentLevels images; specular[0] is the source itself
    std::uint32_t lut_size = kBrdfLutSize;
    std::vector<glm::vec2> brdf_lut;        // lut_size * lut_size; (row = roughness, column = n . v) holds (scale A, bias B)
};

// Derives diffuse SH, the prefiltered levels, and the BRDF table from a radiance image. The source must validate.
[[nodiscard]] Environment build_environment(const EquirectImage& source);

// sRGB transfer functions (IEC 61966-2-1), for values in [0, 1].
[[nodiscard]] float srgb_to_linear(float encoded) noexcept;
[[nodiscard]] float linear_to_srgb(float linear) noexcept;
// Stores a linear value in [0, 1] as an sRGB byte, rounded to the nearest value. Out-of-range input is clamped.
[[nodiscard]] std::uint8_t encode_srgb8(float linear) noexcept;

// Maps a unit direction to equirect coordinates in [0, 1] x [0, 1], and back to the direction at a coordinate.
[[nodiscard]] glm::vec2 direction_to_equirect(const glm::vec3& direction) noexcept;
[[nodiscard]] glm::vec3 equirect_to_direction(const glm::vec2& uv) noexcept;

// Nearest-texel lookup of a radiance image in the direction `direction`.
[[nodiscard]] glm::vec3 sample_equirect(const EquirectImage& image, const glm::vec3& direction) noexcept;
// Irradiance divided by pi at a unit normal, from the SH coefficients.
[[nodiscard]] glm::vec3 evaluate_diffuse(const Environment& environment, const glm::vec3& normal) noexcept;
// Prefiltered radiance around the unit direction `reflection` at `roughness`: the two nearest levels, blended linearly.
[[nodiscard]] glm::vec3 sample_specular(const Environment& environment, const glm::vec3& reflection, float roughness) noexcept;
// The BRDF scale and bias (A, B) for the view angle and roughness.
[[nodiscard]] glm::vec2 sample_brdf_lut(const Environment& environment, float n_dot_v, float roughness) noexcept;

// Inputs of one fragment, all in one space (the scene renderer uses camera-relative world space).
struct SurfaceInput {
    glm::vec3 normal{0.0f, 0.0f, 1.0f};     // unit geometric normal
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};    // unit tangent, perpendicular to the normal
    glm::vec3 bitangent{0.0f, 1.0f, 0.0f};  // unit bitangent, perpendicular to both
    glm::vec3 view{0.0f, 0.0f, 1.0f};       // unit vector from the surface toward the eye
    glm::vec2 uv{0.0f};
    glm::vec4 tint{1.0f};                   // the renderable's color (RenderableRef::color)
    bool front_facing = true;               // false on a back face of a two-sided material
};

// The texture maps a fragment reads. A null map uses the slot's default: white for base color, metallic-roughness, and occlusion; a flat
// normal; and white for emissive, so the emissive factor alone applies.
struct MaterialImages {
    const ImageRgba8* base_color = nullptr;
    const ImageRgba8* metallic_roughness = nullptr;
    const ImageRgba8* normal = nullptr;
    const ImageRgba8* emissive = nullptr;
    const ImageRgba8* occlusion = nullptr;
};

// Linear radiance (unclamped) and coverage of a fragment.
struct ShadedSample {
    glm::vec3 radiance{0.0f};
    float alpha = 1.0f;
};
[[nodiscard]] ShadedSample shade_reference(const SurfaceInput& input, const MaterialParams& params, const MaterialImages& images,
                                           const Environment& environment) noexcept;

// What the color target stores for one fragment. A discarded fragment writes nothing.
struct PixelReference {
    bool discarded = false;
    std::array<std::uint8_t, 4> rgba{};  // sRGB RGB; alpha is linear
};
// Applies the blend mode to a shaded sample: Opaque forces alpha 1, AlphaMask discards below the cutoff and then forces alpha 1, and
// AlphaBlend keeps the coverage.
[[nodiscard]] PixelReference to_pixel(const ShadedSample& sample, const MaterialParams& params) noexcept;

}  // namespace engine::render
