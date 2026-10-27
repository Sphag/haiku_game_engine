#include "engine/render/material.hpp"

#include <cmath>

namespace engine::render {
namespace {

bool finite(float value) {
    return std::isfinite(value);
}

bool in_unit_range(float value) {
    return finite(value) && value >= 0.0f && value <= 1.0f;
}

}  // namespace

std::string_view to_string(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Opaque:
            return "opaque";
        case BlendMode::AlphaMask:
            return "alpha mask";
        case BlendMode::AlphaBlend:
            return "alpha blend";
    }
    return "unknown";
}

std::optional<std::string> validate_material(const MaterialParams& params) {
    const glm::vec4& color = params.base_color;
    if (!in_unit_range(color.r) || !in_unit_range(color.g) || !in_unit_range(color.b) || !in_unit_range(color.a)) {
        return std::string("the base color factor is outside [0, 1]");
    }
    if (!finite(params.emissive.r) || !finite(params.emissive.g) || !finite(params.emissive.b) || glm::any(glm::lessThan(params.emissive, glm::vec3(0.0f)))) {
        return std::string("the emissive factor is negative or not finite");
    }
    if (!in_unit_range(params.metallic)) {
        return std::string("metallic is outside [0, 1]");
    }
    if (!in_unit_range(params.roughness)) {
        return std::string("roughness is outside [0, 1]");
    }
    if (!in_unit_range(params.occlusion_strength)) {
        return std::string("the occlusion strength is outside [0, 1]");
    }
    if (!finite(params.normal_scale) || params.normal_scale < 0.0f) {
        return std::string("the normal scale is negative or not finite");
    }
    if (!in_unit_range(params.alpha_cutoff)) {
        return std::string("the alpha cutoff is outside [0, 1]");
    }
    if (params.blend != BlendMode::Opaque && params.blend != BlendMode::AlphaMask && params.blend != BlendMode::AlphaBlend) {
        return std::string("the blend mode is not one of the defined modes");
    }
    return std::nullopt;
}

MaterialParams resolve(const MaterialParams& parent, const MaterialOverrides& overrides) {
    MaterialParams result = parent;
    if (overrides.base_color) {
        result.base_color = *overrides.base_color;
    }
    if (overrides.emissive) {
        result.emissive = *overrides.emissive;
    }
    if (overrides.metallic) {
        result.metallic = *overrides.metallic;
    }
    if (overrides.roughness) {
        result.roughness = *overrides.roughness;
    }
    if (overrides.occlusion_strength) {
        result.occlusion_strength = *overrides.occlusion_strength;
    }
    if (overrides.normal_scale) {
        result.normal_scale = *overrides.normal_scale;
    }
    if (overrides.alpha_cutoff) {
        result.alpha_cutoff = *overrides.alpha_cutoff;
    }
    if (overrides.blend) {
        result.blend = *overrides.blend;
    }
    if (overrides.two_sided) {
        result.two_sided = *overrides.two_sided;
    }
    return result;
}

std::optional<std::string> validate_image(const ImageRgba8& image) {
    if (image.width == 0 || image.height == 0) {
        return std::string("the image has no pixels");
    }
    const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * 4;
    if (image.pixels.size() != expected) {
        return std::string("the image byte count does not match its size");
    }
    return std::nullopt;
}

}  // namespace engine::render
