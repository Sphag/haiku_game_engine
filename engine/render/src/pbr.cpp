#include "engine/render/pbr.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::render {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Defaults of the texture slots as stored bytes, so the CPU reference and the GPU default textures read the same values.
constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};
constexpr std::array<std::uint8_t, 4> kFlatNormal{128, 128, 255, 255};

float clamp01(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

// Texel of an image at a repeating coordinate, as the shader's Load with fract(uv) computes it. Returns the bytes scaled to [0, 1].
glm::vec4 fetch(const ImageRgba8* image, const glm::vec2& uv, const std::array<std::uint8_t, 4>& fallback) {
    const std::array<std::uint8_t, 4>* bytes = &fallback;
    std::array<std::uint8_t, 4> texel{};
    if (image != nullptr) {
        const float fu = uv.x - std::floor(uv.x);
        const float fv = uv.y - std::floor(uv.y);
        const auto x = std::min(static_cast<std::uint32_t>(std::floor(fu * static_cast<float>(image->width))), image->width - 1);
        const auto y = std::min(static_cast<std::uint32_t>(std::floor(fv * static_cast<float>(image->height))), image->height - 1);
        const std::uint8_t* source = image->texel(x, y);
        std::copy(source, source + 4, texel.begin());
        bytes = &texel;
    }
    return glm::vec4((*bytes)[0], (*bytes)[1], (*bytes)[2], (*bytes)[3]) / 255.0f;
}

// Radical inverse in base 2 (van der Corput), the second coordinate of a Hammersley point.
float radical_inverse(std::uint32_t bits) noexcept {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

glm::vec2 hammersley(std::uint32_t i, std::uint32_t count) noexcept {
    return glm::vec2(static_cast<float>(i) / static_cast<float>(count), radical_inverse(i));
}

// A GGX-distributed half vector in the frame where the normal is +Z. `alpha` is roughness squared.
glm::vec3 importance_ggx(const glm::vec2& xi, float alpha) noexcept {
    const float phi = 2.0f * kPi * xi.x;
    const float cos_theta = std::sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    return glm::vec3(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
}

// Two unit vectors that, with `n`, form an orthonormal frame.
void tangent_frame(const glm::vec3& n, glm::vec3& tangent, glm::vec3& bitangent) noexcept {
    const glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    tangent = glm::normalize(glm::cross(up, n));
    bitangent = glm::cross(n, tangent);
}

// One term of the Schlick-GGX geometry function for the image-based k = alpha / 2.
float geometry_term(float n_dot_x, float k) noexcept {
    return n_dot_x / (n_dot_x * (1.0f - k) + k);
}

// The split-sum scale and bias of one table entry: the integral of GGX over the visible hemisphere, weighted by Fresnel.
glm::vec2 integrate_brdf(float n_dot_v, float roughness) {
    const glm::vec3 v(std::sqrt(std::max(0.0f, 1.0f - n_dot_v * n_dot_v)), 0.0f, n_dot_v);
    const float alpha = roughness * roughness;
    const float k = alpha / 2.0f;
    float scale = 0.0f;
    float bias = 0.0f;
    for (std::uint32_t i = 0; i < kPrefilterSamples; ++i) {
        const glm::vec3 h = importance_ggx(hammersley(i, kPrefilterSamples), alpha);
        const float v_dot_h = glm::dot(v, h);
        const glm::vec3 l = 2.0f * v_dot_h * h - v;
        const float n_dot_l = l.z;
        if (n_dot_l > 0.0f && v_dot_h > 0.0f) {
            const float g_vis = geometry_term(n_dot_v, k) * geometry_term(n_dot_l, k) * v_dot_h / (h.z * n_dot_v);
            const float fresnel = std::pow(1.0f - v_dot_h, 5.0f);
            scale += (1.0f - fresnel) * g_vis;
            bias += fresnel * g_vis;
        }
    }
    const auto count = static_cast<float>(kPrefilterSamples);
    return glm::vec2(scale / count, bias / count);
}

// GGX prefilter with N = V = R: the radiance around the texel's direction, weighted by n . l.
EquirectImage prefilter(const EquirectImage& source, std::uint32_t width, std::uint32_t height, float roughness) {
    EquirectImage out;
    out.width = width;
    out.height = height;
    out.texels.resize(static_cast<std::size_t>(width) * height);
    const float alpha = roughness * roughness;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const glm::vec3 n = equirect_to_direction(glm::vec2((static_cast<float>(x) + 0.5f) / width, (static_cast<float>(y) + 0.5f) / height));
            glm::vec3 tangent;
            glm::vec3 bitangent;
            tangent_frame(n, tangent, bitangent);
            glm::vec3 sum(0.0f);
            float weight = 0.0f;
            for (std::uint32_t i = 0; i < kPrefilterSamples; ++i) {
                const glm::vec3 local = importance_ggx(hammersley(i, kPrefilterSamples), alpha);
                const glm::vec3 h = tangent * local.x + bitangent * local.y + n * local.z;
                const glm::vec3 l = 2.0f * glm::dot(n, h) * h - n;
                const float n_dot_l = glm::dot(n, l);
                if (n_dot_l > 0.0f) {
                    sum += sample_equirect(source, l) * n_dot_l;
                    weight += n_dot_l;
                }
            }
            out.texels[static_cast<std::size_t>(y) * width + x] = weight > 0.0f ? sum / weight : sample_equirect(source, n);
        }
    }
    return out;
}

// Real spherical harmonics, bands 0 to 2, in the order of Environment::diffuse_sh.
std::array<float, 9> sh_basis(const glm::vec3& d) noexcept {
    return {0.282095f,
            0.488603f * d.y,
            0.488603f * d.z,
            0.488603f * d.x,
            1.092548f * d.x * d.y,
            1.092548f * d.y * d.z,
            0.315392f * (3.0f * d.z * d.z - 1.0f),
            1.092548f * d.x * d.z,
            0.546274f * (d.x * d.x - d.y * d.y)};
}

// Cosine-lobe convolution divided by pi (A_l / pi = 1, 2/3, 1/4 for bands 0, 1, 2).
float band_weight(std::size_t index) noexcept {
    if (index == 0) {
        return 1.0f;
    }
    return index < 4 ? 2.0f / 3.0f : 0.25f;
}

}  // namespace

std::optional<std::string> validate_equirect(const EquirectImage& image) {
    if (image.width == 0 || image.height == 0) {
        return std::string("the radiance image has no texels");
    }
    if (image.texels.size() != static_cast<std::size_t>(image.width) * image.height) {
        return std::string("the radiance image texel count does not match its size");
    }
    for (const glm::vec3& texel : image.texels) {
        if (!std::isfinite(texel.r) || !std::isfinite(texel.g) || !std::isfinite(texel.b) || glm::any(glm::lessThan(texel, glm::vec3(0.0f)))) {
            return std::string("a radiance texel is negative or not finite");
        }
    }
    return std::nullopt;
}

Environment build_environment(const EquirectImage& source) {
    Environment environment;

    // Diffuse: project the radiance onto the first nine SH bands, weighting each texel by its solid angle.
    std::array<glm::vec3, 9> coefficients{};
    float total_solid_angle = 0.0f;
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const float theta = (static_cast<float>(y) + 0.5f) / source.height * kPi;
        const float solid_angle = std::sin(theta) * (kPi / source.height) * (2.0f * kPi / source.width);
        total_solid_angle += solid_angle * static_cast<float>(source.width);
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const glm::vec3 direction = equirect_to_direction(glm::vec2((static_cast<float>(x) + 0.5f) / source.width, (static_cast<float>(y) + 0.5f) / source.height));
            const std::array<float, 9> basis = sh_basis(direction);
            const glm::vec3 radiance = source.texels[static_cast<std::size_t>(y) * source.width + x] * solid_angle;
            for (std::size_t i = 0; i < 9; ++i) {
                coefficients[i] += radiance * basis[i];
            }
        }
    }
    // The texel weights sum to the sphere only up to discretization, so scale them to exactly 4 pi. A uniform map then reproduces its radiance.
    const float normalization = 4.0f * kPi / total_solid_angle;
    for (std::size_t i = 0; i < 9; ++i) {
        environment.diffuse_sh[i] = coefficients[i] * (band_weight(i) * normalization);
    }

    // Specular: level 0 is the source; each further level is prefiltered at its roughness, at half the size of the one before.
    environment.specular.push_back(source);
    for (std::uint32_t level = 1; level < kEnvironmentLevels; ++level) {
        const float roughness = static_cast<float>(level) / static_cast<float>(kEnvironmentLevels - 1);
        const std::uint32_t width = std::max(1u, source.width >> level);
        const std::uint32_t height = std::max(1u, source.height >> level);
        environment.specular.push_back(prefilter(source, width, height, roughness));
    }

    environment.lut_size = kBrdfLutSize;
    environment.brdf_lut.resize(static_cast<std::size_t>(kBrdfLutSize) * kBrdfLutSize);
    for (std::uint32_t y = 0; y < kBrdfLutSize; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5f) / kBrdfLutSize;
        for (std::uint32_t x = 0; x < kBrdfLutSize; ++x) {
            const float n_dot_v = (static_cast<float>(x) + 0.5f) / kBrdfLutSize;
            environment.brdf_lut[static_cast<std::size_t>(y) * kBrdfLutSize + x] = integrate_brdf(n_dot_v, roughness);
        }
    }
    return environment;
}

float srgb_to_linear(float encoded) noexcept {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float linear) noexcept {
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t encode_srgb8(float linear) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp01(linear_to_srgb(clamp01(linear))) * 255.0f));
}

glm::vec2 direction_to_equirect(const glm::vec3& direction) noexcept {
    const float u = 0.5f + std::atan2(direction.z, direction.x) / (2.0f * kPi);
    const float v = std::acos(std::clamp(direction.y, -1.0f, 1.0f)) / kPi;
    return glm::vec2(u, v);
}

glm::vec3 equirect_to_direction(const glm::vec2& uv) noexcept {
    const float phi = (uv.x - 0.5f) * 2.0f * kPi;
    const float theta = uv.y * kPi;
    return glm::vec3(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
}

glm::vec3 sample_equirect(const EquirectImage& image, const glm::vec3& direction) noexcept {
    const glm::vec2 uv = direction_to_equirect(direction);
    const auto x = std::min(static_cast<std::uint32_t>(std::floor(uv.x * static_cast<float>(image.width))), image.width - 1);
    const auto y = std::min(static_cast<std::uint32_t>(std::floor(uv.y * static_cast<float>(image.height))), image.height - 1);
    return image.texels[static_cast<std::size_t>(y) * image.width + x];
}

glm::vec3 evaluate_diffuse(const Environment& environment, const glm::vec3& normal) noexcept {
    const std::array<float, 9> basis = sh_basis(normal);
    glm::vec3 sum(0.0f);
    for (std::size_t i = 0; i < 9; ++i) {
        sum += environment.diffuse_sh[i] * basis[i];
    }
    return sum;
}

glm::vec3 sample_specular(const Environment& environment, const glm::vec3& reflection, float roughness) noexcept {
    const float position = clamp01(roughness) * static_cast<float>(kEnvironmentLevels - 1);
    const auto low = std::min(static_cast<std::uint32_t>(std::floor(position)), kEnvironmentLevels - 2);
    const float t = position - static_cast<float>(low);
    const glm::vec3 a = sample_equirect(environment.specular[low], reflection);
    const glm::vec3 b = sample_equirect(environment.specular[low + 1], reflection);
    return a + (b - a) * t;
}

glm::vec2 sample_brdf_lut(const Environment& environment, float n_dot_v, float roughness) noexcept {
    // Bilinear between the four nearest entries, whose centers sit at (i + 0.5) / size, as the shader computes it.
    const auto size = static_cast<float>(environment.lut_size);
    const float fx = clamp01(n_dot_v) * size - 0.5f;
    const float fy = clamp01(roughness) * size - 0.5f;
    const float tx = fx - std::floor(fx);
    const float ty = fy - std::floor(fy);
    const auto last = static_cast<int>(environment.lut_size) - 1;
    const auto clamp_index = [last](float value) { return std::clamp(static_cast<int>(std::floor(value)), 0, last); };
    const int x0 = clamp_index(fx);
    const int x1 = clamp_index(fx + 1.0f);
    const int y0 = clamp_index(fy);
    const int y1 = clamp_index(fy + 1.0f);
    const auto at = [&environment](int x, int y) { return environment.brdf_lut[static_cast<std::size_t>(y) * environment.lut_size + x]; };
    const glm::vec2 top = glm::mix(at(x0, y0), at(x1, y0), tx);
    const glm::vec2 bottom = glm::mix(at(x0, y1), at(x1, y1), tx);
    return glm::mix(top, bottom, ty);
}

ShadedSample shade_reference(const SurfaceInput& input, const MaterialParams& params, const MaterialImages& images,
                             const Environment& environment) noexcept {
    // A back face of a two-sided surface is lit as the surface it shows: its normal and bitangent turn around.
    glm::vec3 normal = input.normal;
    glm::vec3 bitangent = input.bitangent;
    if (params.two_sided && !input.front_facing) {
        normal = -normal;
        bitangent = -bitangent;
    }

    // Tangent-space normal map. Flat texels decode to (0, 0, 1), so an absent map leaves the normal alone.
    glm::vec3 tangent_normal = glm::vec3(fetch(images.normal, input.uv, kFlatNormal)) * 2.0f - 1.0f;
    tangent_normal.x *= params.normal_scale;
    tangent_normal.y *= params.normal_scale;
    tangent_normal = glm::length(tangent_normal) > 0.0f ? glm::normalize(tangent_normal) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 n = glm::normalize(input.tangent * tangent_normal.x + bitangent * tangent_normal.y + normal * tangent_normal.z);

    const glm::vec4 base = fetch(images.base_color, input.uv, kWhite);
    const glm::vec3 albedo = glm::vec3(srgb_to_linear(base.r), srgb_to_linear(base.g), srgb_to_linear(base.b)) *
                             glm::vec3(params.base_color) * glm::vec3(input.tint);
    const float alpha = base.a * params.base_color.a * input.tint.a;

    // Metallic-roughness: green is roughness and blue is metallic, both scaled by their factors.
    const glm::vec4 mr = fetch(images.metallic_roughness, input.uv, kWhite);
    const float roughness = clamp01(mr.g * params.roughness);
    const float metallic = clamp01(mr.b * params.metallic);

    const float occlusion = 1.0f + params.occlusion_strength * (fetch(images.occlusion, input.uv, kWhite).r - 1.0f);
    const glm::vec4 emissive_texel = fetch(images.emissive, input.uv, kWhite);
    const glm::vec3 emissive = glm::vec3(srgb_to_linear(emissive_texel.r), srgb_to_linear(emissive_texel.g), srgb_to_linear(emissive_texel.b)) *
                               params.emissive;

    const glm::vec3 view = glm::normalize(input.view);
    const float n_dot_v = std::max(glm::dot(n, view), 1e-4f);
    const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, metallic);
    const glm::vec2 brdf = sample_brdf_lut(environment, n_dot_v, roughness);
    const glm::vec3 specular_weight = f0 * brdf.x + glm::vec3(brdf.y);

    const glm::vec3 reflection = glm::reflect(-view, n);
    const glm::vec3 diffuse = albedo * (1.0f - metallic) * (glm::vec3(1.0f) - specular_weight) * evaluate_diffuse(environment, n) * occlusion;
    const glm::vec3 specular = sample_specular(environment, reflection, roughness) * specular_weight * occlusion;

    ShadedSample sample;
    sample.radiance = diffuse + specular + emissive;
    sample.alpha = alpha;
    return sample;
}

PixelReference to_pixel(const ShadedSample& sample, const MaterialParams& params) noexcept {
    PixelReference pixel;
    if (params.blend == BlendMode::AlphaMask && sample.alpha < params.alpha_cutoff) {
        pixel.discarded = true;
        return pixel;
    }
    pixel.rgba = {encode_srgb8(sample.radiance.r), encode_srgb8(sample.radiance.g), encode_srgb8(sample.radiance.b), 255};
    if (params.blend == BlendMode::AlphaBlend) {
        pixel.rgba[3] = static_cast<std::uint8_t>(std::lround(clamp01(sample.alpha) * 255.0f));
    }
    return pixel;
}

}  // namespace engine::render
