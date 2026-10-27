#include "engine/render/rt_effects.hpp"

#include "engine/core/result.hpp"
#include "engine/render/render_graph.hpp"
#include "gpu_upload.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::render {

namespace {

using rhi::BufferDesc;
using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::Format;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;
using rhi::TextureDesc;
using rhi::TextureUsage;

// Byte offsets of the parameter block (shaders/rt_effects.hlsl, kParam*).
constexpr std::uint32_t kParamEye = 0;
constexpr std::uint32_t kParamRight = 16;
constexpr std::uint32_t kParamUp = 32;
constexpr std::uint32_t kParamForward = 48;
constexpr std::uint32_t kParamSunDirection = 64;
constexpr std::uint32_t kParamSunRadiance = 80;
constexpr std::uint32_t kParamAmbient = 96;
constexpr std::uint32_t kParamSize = 112;
constexpr std::uint32_t kParamFlags = 120;
constexpr std::uint32_t kParamRoughnessCutoff = 124;
constexpr std::uint32_t kParamReflectionDistance = 128;
constexpr std::uint32_t kParamShadowDistance = 132;
constexpr std::uint32_t kParamBias = 136;
constexpr std::uint32_t kParamPreviousEye = 144;
constexpr std::uint32_t kParamPreviousForward = 160;
constexpr std::uint32_t kParamPreviousRight = 176;
constexpr std::uint32_t kParamPreviousUp = 192;
constexpr std::uint32_t kFlagShadows = 1;
constexpr std::uint32_t kFlagReflections = 2;

// Byte offsets of the instance and triangle records (shaders/rt_effects.hlsl).
constexpr std::uint32_t kInstanceAlbedo = 0;
constexpr std::uint32_t kInstanceMetallic = 12;
constexpr std::uint32_t kInstanceRoughness = 16;
constexpr std::uint32_t kInstanceTriangleBase = 20;
constexpr std::uint32_t kInstanceMasked = 24;
constexpr std::uint32_t kInstanceMaskCutoff = 28;
constexpr std::uint32_t kInstanceMaskRect = 32;
constexpr std::uint32_t kTrianglePositions = 0;
constexpr std::uint32_t kTriangleUvs = 36;

// The dielectric reflectance at normal incidence, as the shader and the caller's PBR shading use.
constexpr float kDielectricF0 = 0.04f;

// The usage of the pass's buffers: read by the shader, and filled by a copy.
constexpr BufferUsage kBufferUsage = BufferUsage::ShaderResource | BufferUsage::CopyDestination;

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

void put_bytes(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const void* source, std::size_t size) {
    std::memcpy(bytes.data() + offset, source, size);
}

void put_float(std::vector<std::uint8_t>& bytes, std::uint32_t offset, float value) {
    put_bytes(bytes, offset, &value, sizeof(value));
}

void put_uint(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t value) {
    put_bytes(bytes, offset, &value, sizeof(value));
}

void put_vec3(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const glm::vec3& value) {
    put_bytes(bytes, offset, &value.x, 3 * sizeof(float));
}

void put_vec4(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const glm::vec4& value) {
    put_bytes(bytes, offset, &value.x, 4 * sizeof(float));
}

// The camera's right vector and the two scaled axes the pass traces with. Shared by the pack and the reference, so both use the same rays.
struct Axes {
    glm::vec3 right;
    glm::vec3 up;
};

Axes scaled_axes(const RtFrame& frame) {
    const glm::vec3 right = glm::normalize(glm::cross(frame.forward, frame.up));
    const float tan_half = std::tan(0.5f * frame.fov_y_radians);
    return Axes{right * (tan_half * frame.aspect), frame.up * tan_half};
}

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}

bool finite(const glm::mat4& m) {
    for (int column = 0; column < 4; ++column) {
        if (!finite(m[column])) {
            return false;
        }
    }
    return true;
}

// The first intersection of a ray with one triangle in [0, t_max], by Moller-Trumbore. The barycentrics follow DXR: the hit is
// (1 - u - v) * v0 + u * v1 + v * v2.
bool intersect_triangle(const glm::vec3& origin, const glm::vec3& direction, const std::array<glm::vec3, 3>& v, float t_max, float& t,
                        glm::vec2& barycentrics) {
    const glm::vec3 edge1 = v[1] - v[0];
    const glm::vec3 edge2 = v[2] - v[0];
    const glm::vec3 p = glm::cross(direction, edge2);
    const float det = glm::dot(edge1, p);
    if (std::abs(det) < 1.0e-12f) {
        return false;
    }
    const float inverse = 1.0f / det;
    const glm::vec3 s = origin - v[0];
    const float u = glm::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, edge1);
    const float w = glm::dot(direction, q) * inverse;
    if (w < 0.0f || u + w > 1.0f) {
        return false;
    }
    const float hit = glm::dot(edge2, q) * inverse;
    if (hit < 0.0f || hit > t_max) {
        return false;
    }
    t = hit;
    barycentrics = glm::vec2(u, w);
    return true;
}

// The world positions of one triangle of an instance.
std::array<glm::vec3, 3> world_positions(const RtInstanceRecord& instance, const RtTriangleRecord& triangle) {
    std::array<glm::vec3, 3> world{};
    for (std::size_t i = 0; i < 3; ++i) {
        world[i] = glm::vec3(instance.transform * glm::vec4(triangle.positions[i], 1.0f));
    }
    return world;
}

// The alpha test of a masked candidate: the UV at the barycentrics, sampled in the instance's rectangle of the mask. The GPU does the
// same arithmetic in AlphaPasses.
bool masked_candidate_passes(const RtInstanceRecord& instance, const RtTriangleRecord& triangle, const glm::vec2& barycentrics,
                             const RtMaskImage& mask) {
    if (!instance.masked) {
        return true;
    }
    const glm::vec2 uv = (1.0f - barycentrics.x - barycentrics.y) * triangle.uvs[0] + barycentrics.x * triangle.uvs[1] +
                         barycentrics.y * triangle.uvs[2];
    const glm::vec2 saturated = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f));
    const float width = static_cast<float>(mask.width);
    const float height = static_cast<float>(mask.height);
    const float texel_x = (instance.mask_rect.x + saturated.x * instance.mask_rect.z) * width;
    const float texel_y = (instance.mask_rect.y + saturated.y * instance.mask_rect.w) * height;
    const auto column = std::clamp(static_cast<std::int64_t>(std::floor(texel_x)), std::int64_t{0}, static_cast<std::int64_t>(mask.width) - 1);
    const auto row = std::clamp(static_cast<std::int64_t>(std::floor(texel_y)), std::int64_t{0}, static_cast<std::int64_t>(mask.height) - 1);
    const float alpha = mask.alpha[static_cast<std::size_t>(row) * mask.width + static_cast<std::size_t>(column)];
    return alpha_passes(alpha, instance.mask_cutoff);
}

struct CpuHit {
    bool hit = false;
    float t = 0.0f;
    std::int32_t instance = -1;
    glm::vec2 barycentrics{0.0f};
    std::array<glm::vec3, 3> world{};
};

// The closest accepted hit in [0, t_max], or the first accepted hit when any_hit is set (a shadow only asks whether something blocks).
CpuHit trace_cpu(const std::vector<RtInstanceRecord>& instances, const std::vector<RtTriangleRecord>& triangles, const RtMaskImage& mask,
                 const glm::vec3& origin, const glm::vec3& direction, float t_max, bool any_hit) {
    CpuHit best;
    best.t = t_max;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const RtInstanceRecord& instance = instances[i];
        for (std::uint32_t k = 0; k < instance.triangle_count; ++k) {
            const RtTriangleRecord& triangle = triangles[instance.triangle_base + k];
            const std::array<glm::vec3, 3> world = world_positions(instance, triangle);
            float t = 0.0f;
            glm::vec2 barycentrics{0.0f};
            if (!intersect_triangle(origin, direction, world, best.t, t, barycentrics)) {
                continue;
            }
            if (!masked_candidate_passes(instance, triangle, barycentrics, mask)) {
                continue;
            }
            best.hit = true;
            best.t = t;
            best.instance = static_cast<std::int32_t>(i);
            best.barycentrics = barycentrics;
            best.world = world;
            if (any_hit) {
                return best;
            }
        }
    }
    return best;
}

struct CpuSurface {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 albedo{0.0f};
    float metallic = 0.0f;
    float roughness = 0.0f;
};

CpuSurface surface_at(const CpuHit& hit, const RtInstanceRecord& instance, const glm::vec3& direction) {
    CpuSurface surface;
    const glm::vec3 w0 = hit.world[0];
    const glm::vec3 w1 = hit.world[1];
    const glm::vec3 w2 = hit.world[2];
    surface.position = w0 + hit.barycentrics.x * (w1 - w0) + hit.barycentrics.y * (w2 - w0);
    const glm::vec3 normal = glm::normalize(glm::cross(w1 - w0, w2 - w0));
    surface.normal = glm::dot(normal, direction) > 0.0f ? -normal : normal;
    surface.albedo = instance.albedo;
    surface.metallic = instance.metallic;
    surface.roughness = instance.roughness;
    return surface;
}

}  // namespace

RtFrame look_at_frame(const glm::vec3& eye, const glm::vec3& target, float fov_y_radians, float aspect) {
    RtFrame frame;
    frame.eye = eye;
    frame.fov_y_radians = fov_y_radians;
    frame.aspect = aspect;
    frame.forward = glm::normalize(target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(frame.forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    frame.up = glm::normalize(glm::cross(right, frame.forward));
    return frame;
}

std::optional<std::string> validate_frame(const RtFrame& frame) {
    if (!finite(frame.eye) || !finite(frame.forward) || !finite(frame.up) || !finite(frame.sun_direction) || !finite(frame.sun_radiance) ||
        !finite(frame.ambient)) {
        return std::string("the frame has a non-finite vector");
    }
    if (!(frame.fov_y_radians > 0.0f && frame.fov_y_radians < 3.14159265f)) {
        return std::string("the vertical field of view must be in (0, pi)");
    }
    if (!(std::isfinite(frame.aspect) && frame.aspect > 0.0f)) {
        return std::string("the aspect ratio must be positive");
    }
    if (std::abs(glm::length(frame.forward) - 1.0f) > 1.0e-4f || std::abs(glm::length(frame.up) - 1.0f) > 1.0e-4f ||
        std::abs(glm::dot(frame.forward, frame.up)) > 1.0e-4f) {
        return std::string("forward and up must be unit vectors and perpendicular");
    }
    if (std::abs(glm::length(frame.sun_direction) - 1.0f) > 1.0e-4f) {
        return std::string("the sun direction must be a unit vector");
    }
    if (glm::any(glm::lessThan(frame.sun_radiance, glm::vec3(0.0f))) || glm::any(glm::lessThan(frame.ambient, glm::vec3(0.0f)))) {
        return std::string("the sun and ambient radiance must not be negative");
    }
    if (!(frame.roughness_cutoff >= 0.0f && frame.roughness_cutoff <= 1.0f)) {
        return std::string("the roughness cutoff must be in [0, 1]");
    }
    if (!(frame.reflection_distance > 0.0f) || !std::isfinite(frame.reflection_distance)) {
        return std::string("the reflection distance must be positive and finite");
    }
    if (!(frame.shadow_distance > 0.0f) || !std::isfinite(frame.shadow_distance)) {
        return std::string("the shadow distance must be positive and finite");
    }
    if (!(frame.bias >= 0.0f) || !std::isfinite(frame.bias)) {
        return std::string("the bias must be zero or positive");
    }
    if (!finite(frame.previous_eye) || !finite(frame.previous_forward) || !finite(frame.previous_up)) {
        return std::string("the previous camera has a non-finite vector");
    }
    if (std::abs(glm::length(frame.previous_forward) - 1.0f) > 1.0e-4f || std::abs(glm::length(frame.previous_up) - 1.0f) > 1.0e-4f ||
        std::abs(glm::dot(frame.previous_forward, frame.previous_up)) > 1.0e-4f) {
        return std::string("the previous forward and up must be unit vectors and perpendicular");
    }
    return std::nullopt;
}

bool reflection_applies(float roughness, float cutoff) noexcept {
    return roughness <= cutoff;
}

bool alpha_passes(float alpha, float cutoff) noexcept {
    return alpha >= cutoff;
}

std::vector<std::uint8_t> pack_frame_parameters(const RtFrame& frame, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes(kRtParameterBytes, 0);
    const Axes axes = scaled_axes(frame);
    put_vec3(bytes, kParamEye, frame.eye);
    put_vec3(bytes, kParamRight, axes.right);
    put_vec3(bytes, kParamUp, axes.up);
    put_vec3(bytes, kParamForward, frame.forward);
    put_vec3(bytes, kParamSunDirection, frame.sun_direction);
    put_vec3(bytes, kParamSunRadiance, frame.sun_radiance);
    put_vec3(bytes, kParamAmbient, frame.ambient);
    put_uint(bytes, kParamSize, width);
    put_uint(bytes, kParamSize + 4, height);
    put_uint(bytes, kParamFlags, (frame.shadows ? kFlagShadows : 0u) | (frame.reflections ? kFlagReflections : 0u));
    put_float(bytes, kParamRoughnessCutoff, frame.roughness_cutoff);
    put_float(bytes, kParamReflectionDistance, frame.reflection_distance);
    put_float(bytes, kParamShadowDistance, frame.shadow_distance);
    put_float(bytes, kParamBias, frame.bias);
    // The previous camera's axes are scaled the same way as the current ones, so the shader projects with one formula.
    RtFrame previous = frame;
    previous.forward = frame.previous_forward;
    previous.up = frame.previous_up;
    const Axes previous_axes = scaled_axes(previous);
    put_vec3(bytes, kParamPreviousEye, frame.previous_eye);
    put_vec3(bytes, kParamPreviousForward, frame.previous_forward);
    put_vec3(bytes, kParamPreviousRight, previous_axes.right);
    put_vec3(bytes, kParamPreviousUp, previous_axes.up);
    return bytes;
}

std::vector<std::uint8_t> pack_instance_table(const std::vector<RtInstanceRecord>& instances) {
    std::vector<std::uint8_t> bytes(instances.size() * kRtInstanceStride, 0);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const RtInstanceRecord& record = instances[i];
        const auto base = static_cast<std::uint32_t>(i * kRtInstanceStride);
        put_vec3(bytes, base + kInstanceAlbedo, record.albedo);
        put_float(bytes, base + kInstanceMetallic, record.metallic);
        put_float(bytes, base + kInstanceRoughness, record.roughness);
        put_uint(bytes, base + kInstanceTriangleBase, record.triangle_base);
        put_uint(bytes, base + kInstanceMasked, record.masked ? 1u : 0u);
        put_float(bytes, base + kInstanceMaskCutoff, record.mask_cutoff);
        put_vec4(bytes, base + kInstanceMaskRect, record.mask_rect);
    }
    return bytes;
}

std::vector<std::uint8_t> pack_triangle_table(const std::vector<RtTriangleRecord>& triangles) {
    std::vector<std::uint8_t> bytes(triangles.size() * kRtTriangleStride, 0);
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const RtTriangleRecord& record = triangles[i];
        const auto base = static_cast<std::uint32_t>(i * kRtTriangleStride);
        for (std::uint32_t k = 0; k < 3; ++k) {
            put_vec3(bytes, base + kTrianglePositions + 12 * k, record.positions[k]);
            put_bytes(bytes, base + kTriangleUvs + 8 * k, &record.uvs[k].x, 2 * sizeof(float));
        }
    }
    return bytes;
}

rhi::TopLevelInstance make_top_level_instance(const RtInstanceRecord& record, std::uint32_t index, rhi::AccelerationStructureHandle bottom_level) {
    rhi::TopLevelInstance instance;
    instance.bottom_level = bottom_level;
    instance.instance_id = index;
    // The 3x4 transform is row-major: entry row * 4 + column is the matrix's element (column, row).
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            instance.transform[static_cast<std::size_t>(row * 4 + column)] = record.transform[column][row];
        }
    }
    return instance;
}

std::optional<std::string> validate_scene_records(const std::vector<RtInstanceRecord>& instances, const std::vector<RtTriangleRecord>& triangles) {
    if (instances.empty()) {
        return std::string("the scene has no instances");
    }
    if (instances.size() >= (1u << 24)) {
        return std::string("the scene has more instances than a top-level structure's 24-bit InstanceID holds");
    }
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const RtInstanceRecord& record = instances[i];
        const std::string where = "instance " + std::to_string(i) + ": ";
        if (record.triangle_count == 0) {
            return where + "a triangle count of zero";
        }
        if (static_cast<std::uint64_t>(record.triangle_base) + record.triangle_count > triangles.size()) {
            return where + "its triangles lie outside the triangle table";
        }
        if (!finite(record.transform)) {
            return where + "the transform is not finite";
        }
        if (!finite(glm::vec4(record.albedo, 1.0f)) || glm::any(glm::lessThan(record.albedo, glm::vec3(0.0f)))) {
            return where + "the albedo must be finite and not negative";
        }
        if (!(record.metallic >= 0.0f && record.metallic <= 1.0f) || !(record.roughness >= 0.0f && record.roughness <= 1.0f)) {
            return where + "metallic and roughness must be in [0, 1]";
        }
        if (record.masked && !(record.mask_cutoff >= 0.0f && record.mask_cutoff <= 1.0f)) {
            return where + "the mask cutoff must be in [0, 1]";
        }
        if (record.masked && (!(record.mask_rect.z > 0.0f) || !(record.mask_rect.w > 0.0f) || !finite(record.mask_rect))) {
            return where + "the mask rectangle must have a positive size";
        }
    }
    return std::nullopt;
}

RtReference trace_reference(const std::vector<RtInstanceRecord>& instances, const std::vector<RtTriangleRecord>& triangles,
                            const RtMaskImage& mask, const RtFrame& frame, std::uint32_t width, std::uint32_t height) {
    RtReference reference;
    reference.width = width;
    reference.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    reference.shadow.assign(count, 1.0f);
    reference.reflection.assign(count, glm::vec4(0.0f));
    reference.primary_instance.assign(count, -1);

    const Axes axes = scaled_axes(frame);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t pixel_index = reference.index(x, y);
            const glm::vec2 pixel = (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / glm::vec2(fw, fh);
            const glm::vec3 direction = glm::normalize(frame.forward + axes.right * (pixel.x * 2.0f - 1.0f) + axes.up * (1.0f - pixel.y * 2.0f));

            const CpuHit primary = trace_cpu(instances, triangles, mask, frame.eye, direction, kRtPrimaryDistance, false);
            if (!primary.hit) {
                continue;
            }
            reference.primary_instance[pixel_index] = primary.instance;
            const RtInstanceRecord& instance = instances[static_cast<std::size_t>(primary.instance)];
            const CpuSurface surface = surface_at(primary, instance, direction);

            if (frame.shadows) {
                const glm::vec3 origin = surface.position + surface.normal * frame.bias;
                const CpuHit blocker = trace_cpu(instances, triangles, mask, origin, frame.sun_direction, frame.shadow_distance, true);
                reference.shadow[pixel_index] = blocker.hit ? 0.0f : 1.0f;
            }

            if (frame.reflections && reflection_applies(surface.roughness, frame.roughness_cutoff)) {
                const glm::vec3 bounce_direction = direction - 2.0f * glm::dot(surface.normal, direction) * surface.normal;
                const glm::vec3 origin = surface.position + surface.normal * frame.bias;
                const CpuHit bounce = trace_cpu(instances, triangles, mask, origin, bounce_direction, frame.reflection_distance, false);
                if (!bounce.hit) {
                    continue;
                }
                const RtInstanceRecord& hit_instance = instances[static_cast<std::size_t>(bounce.instance)];
                const CpuSurface hit_surface = surface_at(bounce, hit_instance, bounce_direction);
                float visible = 1.0f;
                if (frame.shadows) {
                    const glm::vec3 hit_origin = hit_surface.position + hit_surface.normal * frame.bias;
                    if (trace_cpu(instances, triangles, mask, hit_origin, frame.sun_direction, frame.shadow_distance, true).hit) {
                        visible = 0.0f;
                    }
                }
                const glm::vec3 radiance = hit_surface.albedo * (frame.ambient + frame.sun_radiance * (std::max(glm::dot(hit_surface.normal, frame.sun_direction), 0.0f) * visible));
                const glm::vec3 f0 = glm::mix(glm::vec3(kDielectricF0), surface.albedo, surface.metallic);
                const float facing = std::clamp(glm::dot(surface.normal, -direction), 0.0f, 1.0f);
                const glm::vec3 fresnel = f0 + (glm::vec3(1.0f) - f0) * std::pow(1.0f - facing, 5.0f);
                reference.reflection[pixel_index] = glm::vec4(radiance * fresnel, 1.0f);
            }
        }
    }
    return reference;
}

const std::vector<GeometrySupport>& rt_effects_geometry_support() {
    static const std::vector<GeometrySupport> support = {
        {"opaque triangle meshes", true, "rigid instances; the BLAS is opaque, so no candidate test runs"},
        {"alpha-mask triangles", true, "non-opaque BLAS; each candidate is tested against the mask atlas at the instance cutoff"},
        {"alpha-blended triangles", false, "not traced: a blended surface casts no shadow and gives no reflection hit; the caller keeps its raster term"},
        {"skinned and deforming meshes", false, "the triangle table is uploaded once per scene; a refit (US-041) needs a per-frame table update the pass does not do"},
        {"heightfield terrain", true, "terrain tiles are static opaque meshes (make_terrain_render_tile), traced like any opaque mesh; the splat weights are not traced"},
        {"procedural and AABB primitives", false, "the RHI builds triangle bottom-level structures only (US-041)"},
        {"particles, hair, and curves", false, "not triangle geometry; no bottom-level structure is built for them"},
    };
    return support;
}

bool geometry_supported(std::string_view geometry) {
    for (const GeometrySupport& entry : rt_effects_geometry_support()) {
        if (entry.geometry == geometry) {
            return entry.supported;
        }
    }
    return false;
}

RhiResult<std::unique_ptr<RtEffects>> RtEffects::create(rhi::Device& device, std::uint32_t width, std::uint32_t height, rhi::ShaderBinary shader) {
    if (width == 0 || height == 0) {
        return rhi::fail<std::unique_ptr<RtEffects>>(RhiErrorCode::OutOfRange, "ray-traced effects need a non-zero size");
    }
    if (!device.capabilities().enabled(rhi::Feature::RayTracing)) {
        return rhi::fail<std::unique_ptr<RtEffects>>(RhiErrorCode::FeatureDisabled, "ray-traced effects need the RayTracing feature");
    }
    if (shader.bytecode.empty()) {
        return rhi::fail<std::unique_ptr<RtEffects>>(RhiErrorCode::InvalidState, "ray-traced effects need the compute shader (CSRayTracedEffects)");
    }

    // t0 parameters, t1 the scene, t2 instances, t3 triangles, t4 the mask atlas, u5 and u6 the shadow and reflection, and u7 to u9 the
    // primary geometry (depth, normal, motion) for the denoiser. These are the registers the shader reads.
    rhi::DescriptorSetLayoutDesc layout_desc;
    for (std::uint32_t binding = 0; binding <= 4; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    }
    for (std::uint32_t binding = 5; binding <= 9; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    }
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }

    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.kind = rhi::PipelineKind::Compute;
    pipeline_desc.shaders = {std::move(shader)};
    pipeline_desc.set_layouts = {layout.value()};
    pipeline_desc.debug_name = "ray-traced effects";
    RhiResult<rhi::PipelineHandle> pipeline = device.create_pipeline(pipeline_desc);
    if (!pipeline.ok()) {
        return core::failure(pipeline.error());
    }

    RhiResult<rhi::DescriptorSetHandle> set = device.create_descriptor_set(layout.value());
    if (!set.ok()) {
        return core::failure(set.error());
    }

    RhiResult<BufferHandle> parameters = device.create_buffer(buffer_desc(kRtParameterBytes, kBufferUsage, "ray-traced parameters"));
    if (!parameters.ok()) {
        return core::failure(parameters.error());
    }

    TextureDesc output_desc;
    output_desc.width = width;
    output_desc.height = height;
    output_desc.format = Format::R16G16B16A16Float;
    output_desc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::CopySource;
    output_desc.debug_name = "ray-traced shadow";
    RhiResult<rhi::TextureHandle> shadow = device.create_texture(output_desc);
    if (!shadow.ok()) {
        return core::failure(shadow.error());
    }
    output_desc.debug_name = "ray-traced reflection";
    RhiResult<rhi::TextureHandle> reflection = device.create_texture(output_desc);
    if (!reflection.ok()) {
        return core::failure(reflection.error());
    }
    output_desc.debug_name = "ray-traced depth";
    output_desc.format = Format::R32Float;
    RhiResult<rhi::TextureHandle> depth = device.create_texture(output_desc);
    if (!depth.ok()) {
        return core::failure(depth.error());
    }
    output_desc.debug_name = "ray-traced normal";
    output_desc.format = Format::R16G16B16A16Float;
    RhiResult<rhi::TextureHandle> normal = device.create_texture(output_desc);
    if (!normal.ok()) {
        return core::failure(normal.error());
    }
    // Motion is stored as two halves of an RGBA16F target: D3D12 has no typed UAV for R32G32Float, and the offsets are small.
    output_desc.debug_name = "ray-traced motion";
    output_desc.format = Format::R16G16B16A16Float;
    RhiResult<rhi::TextureHandle> motion = device.create_texture(output_desc);
    if (!motion.ok()) {
        return core::failure(motion.error());
    }

    std::unique_ptr<RtEffects> effects(new RtEffects(device, width, height));
    effects->layout_ = layout.value();
    effects->set_ = set.value();
    effects->pipeline_ = pipeline.value();
    effects->parameters_ = parameters.value();
    effects->shadow_ = shadow.value();
    effects->reflection_ = reflection.value();
    effects->depth_ = depth.value();
    effects->normal_ = normal.value();
    effects->motion_ = motion.value();
    if (const RhiStatus written = device.write_buffer(effects->set_, 0, 0, effects->parameters_); !written.ok()) {
        return core::failure(written.error());
    }
    const std::array<rhi::TextureHandle, 5> outputs{effects->shadow_, effects->reflection_, effects->depth_, effects->normal_, effects->motion_};
    for (std::uint32_t i = 0; i < outputs.size(); ++i) {
        if (const RhiStatus written = device.write_texture(effects->set_, 5 + i, 0, outputs[i]); !written.ok()) {
            return core::failure(written.error());
        }
    }
    return core::success(std::move(effects));
}

RtEffects::RtEffects(rhi::Device& device, std::uint32_t width, std::uint32_t height) noexcept : device_(device), width_(width), height_(height) {}

RhiStatus RtEffects::set_scene(const RtScene& scene) {
    if (!scene.top_level.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "ray-traced effects need a built top-level structure");
    }
    if (!scene.mask_atlas.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "ray-traced effects need a mask atlas (a 1x1 opaque texture when nothing is masked)");
    }
    if (const std::optional<std::string> problem = validate_scene_records(scene.instances, scene.triangles)) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "ray-traced scene: " + *problem);
    }

    const std::vector<std::uint8_t> instance_bytes = pack_instance_table(scene.instances);
    const std::vector<std::uint8_t> triangle_bytes = pack_triangle_table(scene.triangles);
    RhiResult<BufferHandle> instances = device_.create_buffer(buffer_desc(instance_bytes.size(), kBufferUsage, "ray-traced instances"));
    if (!instances.ok()) {
        return core::failure(instances.error());
    }
    RhiResult<BufferHandle> triangles = device_.create_buffer(buffer_desc(triangle_bytes.size(), kBufferUsage, "ray-traced triangles"));
    if (!triangles.ok()) {
        return core::failure(triangles.error());
    }
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, instances.value(), kBufferUsage, instance_bytes.size(), instance_bytes, ResourceState::Common); !uploaded.ok()) {
        return uploaded;
    }
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, triangles.value(), kBufferUsage, triangle_bytes.size(), triangle_bytes, ResourceState::Common); !uploaded.ok()) {
        return uploaded;
    }

    // The previous tables are no longer referenced once the descriptors point at the new ones. The RHI keeps their storage until the GPU is done.
    if (instances_.valid()) {
        static_cast<void>(device_.destroy(instances_));
    }
    if (triangles_.valid()) {
        static_cast<void>(device_.destroy(triangles_));
    }
    instances_ = instances.value();
    triangles_ = triangles.value();

    if (const RhiStatus written = device_.write_acceleration_structure(set_, 1, 0, scene.top_level); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(set_, 2, 0, instances_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(set_, 3, 0, triangles_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(set_, 4, 0, scene.mask_atlas); !written.ok()) {
        return written;
    }
    scene_set_ = true;
    return core::success();
}

RhiStatus RtEffects::update_frame(const RtFrame& frame) {
    if (const std::optional<std::string> problem = validate_frame(frame)) {
        return rhi::fail(RhiErrorCode::OutOfRange, "ray-traced frame: " + *problem);
    }
    const std::vector<std::uint8_t> bytes = pack_frame_parameters(frame, width_, height_);
    const ResourceState before = parameters_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, parameters_, kBufferUsage, kRtParameterBytes, bytes, before); !uploaded.ok()) {
        return uploaded;
    }
    parameters_uploaded_ = true;
    frame_set_ = true;
    return core::success();
}

RhiStatus RtEffects::record(rhi::CommandList& list) {
    if (!scene_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "ray-traced effects: set_scene before record");
    }
    if (!frame_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "ray-traced effects: update_frame before record");
    }
    for (const rhi::TextureHandle output : {shadow_, reflection_, depth_, normal_, motion_}) {
        if (const RhiStatus status = list.transition(output, outputs_state_, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
    }
    if (const RhiStatus status = list.set_pipeline(pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, set_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1); !status.ok()) {
        return status;
    }
    for (const rhi::TextureHandle output : {shadow_, reflection_, depth_, normal_, motion_}) {
        if (const RhiStatus status = list.transition(output, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
            return status;
        }
    }
    outputs_state_ = ResourceState::ShaderRead;
    return core::success();
}

RhiStatus RtEffects::release() {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    keep(device_.destroy(set_));
    keep(device_.destroy(layout_));
    keep(device_.destroy(pipeline_));
    keep(device_.destroy(parameters_));
    if (instances_.valid()) {
        keep(device_.destroy(instances_));
    }
    if (triangles_.valid()) {
        keep(device_.destroy(triangles_));
    }
    keep(device_.destroy(shadow_));
    keep(device_.destroy(reflection_));
    keep(device_.destroy(depth_));
    keep(device_.destroy(normal_));
    keep(device_.destroy(motion_));
    scene_set_ = false;
    frame_set_ = false;
    return first;
}

}  // namespace engine::render
