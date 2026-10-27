#include "engine/render/reference_raster.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

std::uint8_t quantize(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

// Screen position of a point. Pixel (0, 0) is the top-left corner, and NDC y points up, as on DX12. A Vulkan shader flips y first, so the
// two backends meet here.
glm::vec2 to_screen(const glm::vec3& ndc, float width, float height) {
    return glm::vec2((ndc.x * 0.5f + 0.5f) * width, (1.0f - ndc.y) * 0.5f * height);
}

// Twice the signed area of the triangle (a, b, p), positive when p is on the left of the edge a to b.
float edge(const glm::vec2& a, const glm::vec2& b, const glm::vec2& p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Linear blend skinning in object space, as the GPU does it: the weighted sum of each joint's palette applied to the bind position.
glm::vec3 skin(const glm::vec3& position, const glm::uvec4& joints, const glm::vec4& weights, const glm::mat4* palette, std::uint32_t bones) {
    glm::vec3 out(0.0f);
    for (int i = 0; i < 4; ++i) {
        if (weights[i] > 0.0f && joints[i] < bones) {
            out += weights[i] * glm::vec3(palette[joints[i]] * glm::vec4(position, 1.0f));
        }
    }
    return out;
}

}  // namespace

SceneImage rasterize_reference(const FrameSnapshot& snapshot, const std::vector<MeshData>& meshes, std::uint32_t width,
                               std::uint32_t height, const MotionHistory& previous) {
    SceneImage image;
    image.width = width;
    image.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    image.color.assign(count, std::array<std::uint8_t, 4>{0, 0, 0, 0});
    image.pick_ids.assign(count, 0u);
    image.depth.assign(count, 0.0f);
    image.velocity.assign(count, glm::vec2(0.0f));
    std::vector<float> depth_test(count, 1.0f);  // the depth buffer is cleared to the far plane
    if (width == 0 || height == 0) {
        return image;
    }

    // The previous frame's clip matrix for a model relative to the current camera: the previous camera's view, moved by the camera's motion.
    const glm::vec3 camera_motion = glm::vec3(snapshot.camera().position - previous.camera_position);
    const glm::mat4 previous_view_projection = previous.view_projection * glm::translate(glm::mat4(1.0f), camera_motion);

    const float w_pixels = static_cast<float>(width);
    const float h_pixels = static_cast<float>(height);
    for (const SnapshotItem& item : snapshot.items()) {
        if (item.pick_id == 0) {
            continue;  // shadow-only: not in the view
        }
        const MeshData& mesh = meshes[item.mesh];
        const glm::mat4 clip_from_object = snapshot.view_projection() * item.model;
        const glm::mat4 previous_from_object = previous_view_projection * item.previous_model;
        std::vector<glm::vec4> clip;
        std::vector<glm::vec4> clip_previous;
        clip.reserve(mesh.positions.size());
        clip_previous.reserve(mesh.positions.size());
        const bool skinned = item.bone_count > 0 && !mesh.joints.empty();
        for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
            if (skinned) {
                const glm::mat4* palette = snapshot.palettes().data() + item.palette;
                const glm::mat4* previous_palette = snapshot.previous_palettes().data() + item.palette;
                const glm::vec3 posed = skin(mesh.positions[v], mesh.joints[v], mesh.weights[v], palette, item.bone_count);
                const glm::vec3 posed_previous = skin(mesh.positions[v], mesh.joints[v], mesh.weights[v], previous_palette, item.bone_count);
                clip.push_back(clip_from_object * glm::vec4(posed, 1.0f));
                clip_previous.push_back(previous_from_object * glm::vec4(posed_previous, 1.0f));
            } else {
                clip.push_back(clip_from_object * glm::vec4(mesh.positions[v], 1.0f));
                clip_previous.push_back(previous_from_object * glm::vec4(mesh.positions[v], 1.0f));
            }
        }
        const std::array<std::uint8_t, 4> rgba{quantize(item.color.r), quantize(item.color.g), quantize(item.color.b),
                                               quantize(item.color.a)};

        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const std::uint32_t corner_index[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
            const glm::vec4* corners[3] = {&clip[corner_index[0]], &clip[corner_index[1]], &clip[corner_index[2]]};
            if (corners[0]->w <= 0.0f || corners[1]->w <= 0.0f || corners[2]->w <= 0.0f) {
                continue;
            }
            glm::vec3 ndc[3];
            glm::vec2 screen[3];
            for (int i = 0; i < 3; ++i) {
                ndc[i] = glm::vec3(*corners[i]) / corners[i]->w;
                screen[i] = to_screen(ndc[i], w_pixels, h_pixels);
            }
            const float area = edge(screen[0], screen[1], screen[2]);
            if (area == 0.0f) {
                continue;
            }

            // Pixels whose centers fall inside the triangle's box.
            const int min_x = std::max(0, static_cast<int>(std::floor(std::min({screen[0].x, screen[1].x, screen[2].x}))));
            const int max_x = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(std::max({screen[0].x, screen[1].x, screen[2].x}))));
            const int min_y = std::max(0, static_cast<int>(std::floor(std::min({screen[0].y, screen[1].y, screen[2].y}))));
            const int max_y = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(std::max({screen[0].y, screen[1].y, screen[2].y}))));
            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    const glm::vec2 center(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                    const float w0 = edge(screen[1], screen[2], center) / area;
                    const float w1 = edge(screen[2], screen[0], center) / area;
                    const float w2 = edge(screen[0], screen[1], center) / area;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;
                    }
                    // NDC depth is affine in screen space, so barycentric weights interpolate it exactly.
                    const float z = w0 * ndc[0].z + w1 * ndc[1].z + w2 * ndc[2].z;
                    const std::size_t pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
                    // Depth lies in [0, 1]; the test is LESS against the cleared value, as on the GPU.
                    if (z < 0.0f || z >= depth_test[pixel]) {
                        continue;
                    }
                    depth_test[pixel] = z;
                    image.color[pixel] = rgba;
                    image.pick_ids[pixel] = item.pick_id;
                    image.depth[pixel] = z;

                    // Clip-space attributes are interpolated with perspective correction, as the GPU does for the shader's varyings. The
                    // motion vector is then the difference of the two projected positions the varyings give.
                    const float bary[3] = {w0 / corners[0]->w, w1 / corners[1]->w, w2 / corners[2]->w};
                    const float norm = bary[0] + bary[1] + bary[2];
                    glm::vec4 now(0.0f);
                    glm::vec4 before(0.0f);
                    for (int i = 0; i < 3; ++i) {
                        now += (bary[i] / norm) * *corners[i];
                        before += (bary[i] / norm) * clip_previous[corner_index[i]];
                    }
                    const glm::vec2 now_ndc = glm::vec2(now) / now.w;
                    const glm::vec2 before_ndc = glm::vec2(before) / before.w;
                    image.velocity[pixel] = (now_ndc - before_ndc) * glm::vec2(0.5f, -0.5f);
                }
            }
        }
    }
    return image;
}

SceneImage rasterize_reference(const FrameSnapshot& snapshot, const std::vector<MeshData>& meshes, std::uint32_t width,
                               std::uint32_t height) {
    return rasterize_reference(snapshot, meshes, width, height, MotionHistory{snapshot.view_projection(), snapshot.camera().position});
}

}  // namespace engine::render
