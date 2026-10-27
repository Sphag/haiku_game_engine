#pragma once

// Shared helpers for the physically based material tests (US-034). The material scenes are flat quads in planes perpendicular to the view
// axis, so the expected pixels can be worked out exactly: each pixel's ray is intersected with the quad's plane to get the surface point,
// and the CPU reference (shade_reference) shades that point. The GPU must match that to within a few 8-bit steps.

#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/pbr.hpp"
#include "engine/render/scene_image.hpp"

#include "engine/scene/scene_graph.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace engine::render::tests {

inline constexpr std::uint32_t kMaterialSize = 64;

// A camera at the origin looking down -Z with a 60 degree field, so camera-relative and world coordinates coincide.
inline Camera material_camera() {
    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 0.0);
    camera.fov_y_radians = 1.0471975511965976;  // 60 degrees
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    return camera;
}

// The radiance of a sun over a gradient sky, on a 64 x 32 equirect map. The sun is bright enough to show as a highlight on mirror-like surfaces.
inline EquirectImage sun_sky(float sun_strength = 6.0f) {
    EquirectImage image;
    image.width = 64;
    image.height = 32;
    image.texels.resize(static_cast<std::size_t>(image.width) * image.height);
    // The sun is a smooth lobe, not a hard disc. A nearest-texel lookup then changes little where a ray crosses a texel boundary, so the
    // GPU and the CPU reference agree to a few 8-bit steps.
    const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const glm::vec3 d = equirect_to_direction(glm::vec2((static_cast<float>(x) + 0.5f) / image.width, (static_cast<float>(y) + 0.5f) / image.height));
            glm::vec3 radiance = glm::mix(glm::vec3(0.15f, 0.25f, 0.45f), glm::vec3(0.7f, 0.6f, 0.5f), d.y * 0.5f + 0.5f);
            radiance += glm::vec3(sun_strength) * std::pow(std::max(glm::dot(d, sun), 0.0f), 40.0f);
            image.texels[static_cast<std::size_t>(y) * image.width + x] = radiance;
        }
    }
    return image;
}

inline EquirectImage uniform_radiance(float value) {
    EquirectImage image;
    image.width = 16;
    image.height = 8;
    image.texels.assign(static_cast<std::size_t>(image.width) * image.height, glm::vec3(value));
    return image;
}

// A quad that lies in a plane through `center`. Its normal, tangent, and bitangent say which side faces the viewer; the UVs run from (0, 0)
// at the top left of the quad to (1, 1) at the bottom right, as make_plane_mesh and reversed_plane lay them out.
struct QuadSurface {
    glm::vec3 center{0.0f};
    float half = 1.5f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    glm::vec3 bitangent{0.0f, -1.0f, 0.0f};
};

// The mesh of a quad facing +Z, or of one facing -Z (the same square with reversed winding and normals).
inline MeshData quad_mesh(float half, bool faces_away) {
    MeshData mesh = make_plane_mesh(half);
    if (faces_away) {
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
        }
        for (glm::vec3& normal : mesh.normals) {
            normal = -normal;
        }
    }
    return mesh;
}

// The shading inputs of the surface that pixel (x, y) shows. The ray comes from the eye at the origin through the pixel center; it is
// intersected with the quad's plane. `view_projection` is the snapshot's, which is the same on both backends.
inline SurfaceInput quad_input(const glm::mat4& view_projection, const QuadSurface& quad, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                               std::uint32_t height, const glm::vec4& tint, bool front_facing) {
    const glm::vec2 ndc((static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f, 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f);
    const glm::vec4 far_point = glm::inverse(view_projection) * glm::vec4(ndc, 1.0f, 1.0f);
    const glm::vec3 direction = glm::normalize(glm::vec3(far_point) / far_point.w);
    const float t = glm::dot(quad.center, quad.normal) / glm::dot(direction, quad.normal);
    const glm::vec3 point = t * direction;

    const glm::vec3 local = point - quad.center;
    SurfaceInput input;
    input.normal = quad.normal;
    input.tangent = quad.tangent;
    input.bitangent = quad.bitangent;
    input.view = -glm::normalize(point);
    input.uv = glm::vec2((glm::dot(local, quad.tangent) + quad.half) / (2.0f * quad.half),
                         (quad.half + glm::dot(local, quad.bitangent)) / (2.0f * quad.half));
    input.tint = tint;
    input.front_facing = front_facing;
    return input;
}

// True when the pixel's ray hits the quad itself, not just its plane.
inline bool covers(const glm::mat4& view_projection, const QuadSurface& quad, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
    const glm::vec2 ndc((static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f, 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f);
    const glm::vec4 far_point = glm::inverse(view_projection) * glm::vec4(ndc, 1.0f, 1.0f);
    const glm::vec3 direction = glm::normalize(glm::vec3(far_point) / far_point.w);
    const float t = glm::dot(quad.center, quad.normal) / glm::dot(direction, quad.normal);
    const glm::vec3 local = t * direction - quad.center;
    return std::abs(glm::dot(local, quad.tangent)) <= quad.half && std::abs(glm::dot(local, quad.bitangent)) <= quad.half;
}

// The 8-bit pixel the CPU reference predicts for the surface at (x, y).
inline PixelReference expected_pixel(const Environment& environment, const SurfaceInput& input, const MaterialParams& params,
                                     const MaterialImages& images) {
    return to_pixel(shade_reference(input, params, images, environment), params);
}

// A plain image of one color.
inline ImageRgba8 solid_image(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    ImageRgba8 image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = r;
        image.pixels[i + 1] = g;
        image.pixels[i + 2] = b;
        image.pixels[i + 3] = a;
    }
    return image;
}

// Sets the texel at (x, y) of an image.
inline void set_texel(ImageRgba8& image, std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    std::uint8_t* texel = image.pixels.data() + (static_cast<std::size_t>(y) * image.width + x) * 4;
    texel[0] = r;
    texel[1] = g;
    texel[2] = b;
    texel[3] = a;
}

}  // namespace engine::render::tests
