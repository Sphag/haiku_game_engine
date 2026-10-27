// Physically based materials on real GPUs (US-034). Each scene is a few quads, and every pixel is compared with the CPU reference (shade_reference)
// evaluated on the surface that pixel shows. The scenes cover the metallic and roughness extremes, sRGB color spaces, alpha-mask edges,
// alpha-blend ordering, two-sided culling, occlusion and emissive maps, normal maps, material instances, and an environment change. Debug
// layers are on, and any validation error fails the check.

#include "render_material_fixture.hpp"

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/pbr.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using engine::render::BlendMode;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::ImageRgba8;
using engine::render::MaterialImages;
using engine::render::MaterialOverrides;
using engine::render::MaterialParams;
using engine::render::MaterialTextures;
using engine::render::MeshData;
using engine::render::PixelReference;
using engine::render::RenderableRef;
using engine::render::resolve;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::tests::expected_pixel;
using engine::render::tests::kMaterialSize;
using engine::render::tests::material_camera;
using engine::render::tests::QuadSurface;
using engine::render::tests::quad_input;
using engine::render::tests::quad_mesh;
using engine::render::tests::set_texel;
using engine::render::tests::solid_image;
using engine::render::tests::sun_sky;
using engine::render::tests::uniform_radiance;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::scene::Transform;

namespace {

constexpr std::uint32_t kSize = kMaterialSize;

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

void require_ok(const RhiStatus& status) {
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    return desc;
}

// DX12 loads the DXIL outputs and Vulkan loads the SPIR-V outputs of the same HLSL entry points (see tests/CMakeLists.txt).
std::vector<std::uint8_t> load_shader(Backend backend, const std::string& name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + name + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

SceneShaders shaders_for(Backend backend) {
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    return shaders;
}

// One device, one renderer, and a scene of quads. Quads are placed in renderable order, so pick id n + 1 is the n-th placed quad.
class Harness {
public:
    explicit Harness(Backend backend, bool set_env = true)
        : backend_(backend), device_(take(engine::rhi::create_device(debug_device(backend)))) {
        SceneRendererDesc desc;
        desc.width = kSize;
        desc.height = kSize;
        desc.max_items = 64;
        renderer_ = take(SceneRenderer::create(*device_, desc, shaders_for(backend)));
        if (set_env) {
            require_ok(renderer_->set_environment(sun_sky()));
        }
    }

    ~Harness() {
        static_cast<void>(renderer_->release());
    }

    std::uint32_t upload(const MeshData& mesh) {
        return take(renderer_->upload_mesh(mesh));
    }

    std::uint32_t texture(const ImageRgba8& image) {
        return take(renderer_->upload_texture(image));
    }

    std::uint32_t material(const MaterialParams& params, const MaterialTextures& textures = {}) {
        return take(renderer_->create_material(params, textures));
    }

    // Places a renderable at the quad's center. The quad's mesh and the surface description must agree.
    void place(std::uint32_t mesh, std::uint32_t material_id, const glm::vec3& center, const glm::vec4& tint = glm::vec4(1.0f)) {
        Transform transform;
        transform.position = glm::dvec3(center);
        RenderableRef renderable;
        renderable.node = graph_.create(transform).value();
        renderable.mesh = mesh;
        renderable.color = tint;
        renderable.material = material_id;
        renderables_.push_back(renderable);
    }

    // Extracts and draws the scene, then checks the debug layers. Returns the snapshot the image was drawn from.
    FrameSnapshot draw() {
        FrameSnapshot snapshot = extract_frame(graph_, material_camera(), renderer_->bounds(), renderables_);
        require_ok(renderer_->render(snapshot));
        require_ok(device_->check_debug_messages());
        return snapshot;
    }

    void clear_renderables() {
        renderables_.clear();
    }

    SceneRenderer& renderer() {
        return *renderer_;
    }

    Backend backend() const {
        return backend_;
    }

private:
    Backend backend_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<SceneRenderer> renderer_;
    engine::scene::SceneGraph graph_;
    std::vector<RenderableRef> renderables_;
};

// Compares one pixel with the reference. A discarded fragment must leave no pick and no color; any other must match within two 8-bit steps.
// Returns true on a match, so a scene can count its mismatches.
bool pixel_matches(const SceneImage& image, std::uint32_t x, std::uint32_t y, const PixelReference& expected, std::uint32_t pick) {
    const std::size_t i = image.index(x, y);
    if (expected.discarded) {
        return image.pick_ids[i] == 0 && image.color[i] == std::array<std::uint8_t, 4>{0, 0, 0, 0};
    }
    if (image.pick_ids[i] != pick) {
        return false;
    }
    for (std::size_t c = 0; c < 4; ++c) {
        const int difference = static_cast<int>(image.color[i][c]) - static_cast<int>(expected.rgba[c]);
        if (std::abs(difference) > 2) {
            return false;
        }
    }
    return true;
}

void check_pixel(const SceneImage& image, std::uint32_t x, std::uint32_t y, const PixelReference& expected, std::uint32_t pick) {
    const std::size_t i = image.index(x, y);
    INFO("pixel " << x << "," << y << " got " << static_cast<int>(image.color[i][0]) << "," << static_cast<int>(image.color[i][1]) << ","
                  << static_cast<int>(image.color[i][2]) << "," << static_cast<int>(image.color[i][3]) << " pick " << image.pick_ids[i]);
    INFO("expected " << static_cast<int>(expected.rgba[0]) << "," << static_cast<int>(expected.rgba[1]) << "," << static_cast<int>(expected.rgba[2]) << ","
                     << static_cast<int>(expected.rgba[3]) << (expected.discarded ? " (discarded)" : ""));
    CHECK(pixel_matches(image, x, y, expected, pick));
}

// Checks every pixel of the frame against a per-pixel expectation. `expect(x, y)` returns the expected pixel and its pick id.
// A nearest-texel environment lookup can land on the other side of a texel boundary on the GPU than on the CPU, when a ray is within float
// rounding of the boundary. Such a pixel differs by a few steps, so the check allows a handful of them, each within kBoundaryError steps.
// Every other pixel must match within two steps.
constexpr int kBoundaryError = 8;
constexpr std::size_t kBoundaryPixels = 4;

template <typename Expect>
void check_every_pixel(const SceneImage& image, Expect expect) {
    std::size_t mismatches = 0;
    std::size_t boundary = 0;
    int worst = 0;
    std::string first;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const auto [expected, pick] = expect(x, y);
            if (!expected.discarded && image.pick_ids[image.index(x, y)] == pick) {
                for (std::size_t c = 0; c < 4; ++c) {
                    const int difference = std::abs(static_cast<int>(image.color[image.index(x, y)][c]) - static_cast<int>(expected.rgba[c]));
                    worst = std::max(worst, difference);
                }
            }
            if (!pixel_matches(image, x, y, expected, pick)) {
                if (!expected.discarded && image.pick_ids[image.index(x, y)] == pick && boundary < kBoundaryPixels) {
                    // Counted as a boundary pixel below if its error is small enough.
                    ++boundary;
                    continue;
                }
                if (mismatches == 0) {
                    const std::size_t i = image.index(x, y);
                    first = "first mismatch at " + std::to_string(x) + "," + std::to_string(y) + ": got " + std::to_string(image.color[i][0]) + "," +
                            std::to_string(image.color[i][1]) + "," + std::to_string(image.color[i][2]) + "," + std::to_string(image.color[i][3]) +
                            " pick " + std::to_string(image.pick_ids[i]) + "; expected " + std::to_string(expected.rgba[0]) + "," +
                            std::to_string(expected.rgba[1]) + "," + std::to_string(expected.rgba[2]) + "," + std::to_string(expected.rgba[3]) +
                            " pick " + std::to_string(pick) + (expected.discarded ? " (discarded)" : "");
                }
                ++mismatches;
            }
        }
    }
    INFO(first);
    CHECK(mismatches == 0);
    CHECK(boundary <= kBoundaryPixels);
    CHECK(worst <= kBoundaryError);
}

// Quads that cover the whole frame at depth `z`. Half the frame's height at depth d is d * tan(30 degrees), so a half size of 7 covers depths up to 12.
QuadSurface full_frame_quad(float z, float half = 7.0f) {
    QuadSurface quad;
    quad.center = glm::vec3(0.0f, 0.0f, z);
    quad.half = half;
    return quad;
}

// ---- Scenes ----

void run_metal_rough_extremes(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    // Metallic and roughness at their extremes: dielectric and metal, each smooth and fully rough.
    const float metallic[] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float roughness[] = {0.0f, 1.0f, 0.0f, 1.0f};
    const glm::vec3 centers[] = {{-1.5f, 1.5f, -5.0f}, {1.5f, 1.5f, -5.0f}, {-1.5f, -1.5f, -5.0f}, {1.5f, -1.5f, -5.0f}};
    std::vector<QuadSurface> quads;
    for (std::size_t q = 0; q < 4; ++q) {
        MaterialParams params;
        params.base_color = glm::vec4(0.8f, 0.4f, 0.2f, 1.0f);
        params.metallic = metallic[q];
        params.roughness = roughness[q];
        scene.place(mesh, scene.material(params), centers[q]);
        QuadSurface quad;
        quad.center = centers[q];
        quad.half = 1.5f;
        quads.push_back(quad);
    }
    const FrameSnapshot snapshot = scene.draw();
    REQUIRE(snapshot.stats().drawn == 4);

    const std::uint32_t pixels[][2] = {{16, 16}, {47, 16}, {16, 47}, {47, 47}};
    for (std::size_t q = 0; q < 4; ++q) {
        MaterialParams params;
        params.base_color = glm::vec4(0.8f, 0.4f, 0.2f, 1.0f);
        params.metallic = metallic[q];
        params.roughness = roughness[q];
        const auto input = quad_input(snapshot.view_projection(), quads[q], pixels[q][0], pixels[q][1], kSize, kSize, glm::vec4(1.0f), true);
        check_pixel(scene.renderer().image(), pixels[q][0], pixels[q][1], expected_pixel(scene.renderer().environment(), input, params, {}),
                    static_cast<std::uint32_t>(q + 1));
    }
    // The whole quads, not only their centers, match the reference.
    check_every_pixel(scene.renderer().image(), [&](std::uint32_t x, std::uint32_t y) {
        for (std::size_t q = 0; q < 4; ++q) {
            const bool inside = (x < 32) == (centers[q].x < 0.0f) && (y < 32) == (centers[q].y > 0.0f);
            if (inside) {
                MaterialParams params;
                params.base_color = glm::vec4(0.8f, 0.4f, 0.2f, 1.0f);
                params.metallic = metallic[q];
                params.roughness = roughness[q];
                const auto input = quad_input(snapshot.view_projection(), quads[q], x, y, kSize, kSize, glm::vec4(1.0f), true);
                return std::make_pair(expected_pixel(scene.renderer().environment(), input, params, {}), static_cast<std::uint32_t>(q + 1));
            }
        }
        return std::make_pair(PixelReference{true, {}}, 0u);
    });
}

void run_color_space(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    // A base color texel must be decoded from sRGB before lighting. The reference applies the decode, so a shader that read the texel as
    // linear would miss it by many 8-bit steps at this gray level.
    ImageRgba8 base = solid_image(2, 2, 128, 128, 128, 255);
    MaterialTextures textures;
    textures.base_color = scene.texture(base);
    MaterialParams params;
    params.roughness = 1.0f;
    scene.place(mesh, scene.material(params, textures), glm::vec3(0.0f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    MaterialImages images;
    images.base_color = &base;
    QuadSurface quad;
    quad.center = glm::vec3(0.0f, 0.0f, -5.0f);
    quad.half = 1.5f;
    const auto input = quad_input(snapshot.view_projection(), quad, 32, 32, kSize, kSize, glm::vec4(1.0f), true);
    const PixelReference expected = expected_pixel(scene.renderer().environment(), input, params, images);
    check_pixel(scene.renderer().image(), 32, 32, expected, 1);
}

void run_alpha_mask_edges(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(7.0f, false));
    // The texels alternate between opaque and transparent in a checkerboard. The edges fall on the middle of the frame, so each pixel
    // reads one texel. Cutoff 0.5 keeps the opaque texels and discards the others, which then leave no pick, color, or depth.
    ImageRgba8 base = solid_image(2, 2, 255, 128, 0, 0);
    set_texel(base, 0, 0, 255, 128, 0, 255);
    set_texel(base, 1, 1, 255, 128, 0, 255);
    MaterialTextures textures;
    textures.base_color = scene.texture(base);
    MaterialParams params;
    params.blend = BlendMode::AlphaMask;
    params.alpha_cutoff = 0.5f;
    params.roughness = 1.0f;
    scene.place(mesh, scene.material(params, textures), glm::vec3(0.0f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    MaterialImages images;
    images.base_color = &base;
    const QuadSurface quad = full_frame_quad(-5.0f);
    check_every_pixel(scene.renderer().image(), [&](std::uint32_t x, std::uint32_t y) {
        const auto input = quad_input(snapshot.view_projection(), quad, x, y, kSize, kSize, glm::vec4(1.0f), true);
        const PixelReference expected = expected_pixel(scene.renderer().environment(), input, params, images);
        return std::make_pair(expected, expected.discarded ? 0u : 1u);
    });
    // The four pixels at the frame center read the four texels around the texel corner.
    const SceneImage& image = scene.renderer().image();
    CHECK(image.pick_ids[image.index(31, 31)] == 1);
    CHECK(image.pick_ids[image.index(32, 31)] == 0);
    CHECK(image.pick_ids[image.index(31, 32)] == 0);
    CHECK(image.pick_ids[image.index(32, 32)] == 1);
}

// Two translucent quads over an opaque floor. `front_first` submits the near quad first; the blended result must not change.
void run_alpha_blend_order(Backend backend, bool front_first) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(7.0f, false));
    MaterialParams floor_params;
    floor_params.roughness = 1.0f;
    MaterialParams blue;
    blue.base_color = glm::vec4(0.0f, 0.0f, 1.0f, 0.5f);
    blue.blend = BlendMode::AlphaBlend;
    blue.roughness = 1.0f;
    MaterialParams red = blue;
    red.base_color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
    const std::uint32_t floor_id = scene.material(floor_params);
    const std::uint32_t blue_id = scene.material(blue);
    const std::uint32_t red_id = scene.material(red);

    scene.place(mesh, floor_id, glm::vec3(0.0f, 0.0f, -10.0f));
    if (front_first) {
        scene.place(mesh, red_id, glm::vec3(0.0f, 0.0f, -5.0f));
        scene.place(mesh, blue_id, glm::vec3(0.0f, 0.0f, -7.0f));
    } else {
        scene.place(mesh, blue_id, glm::vec3(0.0f, 0.0f, -7.0f));
        scene.place(mesh, red_id, glm::vec3(0.0f, 0.0f, -5.0f));
    }
    const FrameSnapshot snapshot = scene.draw();
    REQUIRE(snapshot.stats().drawn == 3);

    // Expected: the floor, then the far blue layer over it, then the near red layer over that. Each layer's coverage is its alpha, which the
    // target stores as a byte, so the reference reads the same byte. The blend runs on the encoded 8-bit values, as the GPU does.
    check_every_pixel(scene.renderer().image(), [&](std::uint32_t x, std::uint32_t y) {
        const auto floor_in = quad_input(snapshot.view_projection(), full_frame_quad(-10.0f), x, y, kSize, kSize, glm::vec4(1.0f), true);
        const PixelReference floor_pixel = expected_pixel(scene.renderer().environment(), floor_in, floor_params, {});
        const auto blue_in = quad_input(snapshot.view_projection(), full_frame_quad(-7.0f), x, y, kSize, kSize, glm::vec4(1.0f), true);
        const PixelReference back_layer = expected_pixel(scene.renderer().environment(), blue_in, blue, {});
        const auto red_in = quad_input(snapshot.view_projection(), full_frame_quad(-5.0f), x, y, kSize, kSize, glm::vec4(1.0f), true);
        const PixelReference front_layer = expected_pixel(scene.renderer().environment(), red_in, red, {});
        PixelReference result;
        for (std::size_t c = 0; c < 3; ++c) {
            const float a_back = back_layer.rgba[3] / 255.0f;
            const float a_front = front_layer.rgba[3] / 255.0f;
            const float after_back = back_layer.rgba[c] * a_back + floor_pixel.rgba[c] * (1.0f - a_back);
            const float after_front = front_layer.rgba[c] * a_front + after_back * (1.0f - a_front);
            result.rgba[c] = static_cast<std::uint8_t>(std::lround(after_front));
        }
        result.rgba[3] = 255;
        return std::make_pair(result, floor_pixel.discarded ? 0u : 1u);
    });
}

void run_two_sided(Backend backend) {
    Harness scene(backend);
    const std::uint32_t one_sided = scene.upload(quad_mesh(1.5f, true));
    MaterialParams culled;
    culled.roughness = 1.0f;
    MaterialParams both = culled;
    both.two_sided = true;
    both.base_color = glm::vec4(0.2f, 0.9f, 0.3f, 1.0f);
    // The left quad faces away from the camera and is one-sided, so the back face is culled. The right quad is the same geometry, two-sided.
    scene.place(one_sided, scene.material(culled), glm::vec3(-1.5f, 0.0f, -5.0f));
    scene.place(one_sided, scene.material(both), glm::vec3(1.5f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    QuadSurface away;
    away.center = glm::vec3(1.5f, 0.0f, -5.0f);
    away.half = 1.5f;
    away.normal = glm::vec3(0.0f, 0.0f, -1.0f);
    away.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
    away.bitangent = glm::vec3(0.0f, -1.0f, 0.0f);
    check_every_pixel(scene.renderer().image(), [&](std::uint32_t x, std::uint32_t y) {
        if (x < 32 || !engine::render::tests::covers(snapshot.view_projection(), away, x, y, kSize, kSize)) {
            return std::make_pair(PixelReference{true, {}}, 0u);
        }
        const auto input = quad_input(snapshot.view_projection(), away, x, y, kSize, kSize, glm::vec4(1.0f), false);
        return std::make_pair(expected_pixel(scene.renderer().environment(), input, both, {}), 2u);
    });
}

void run_occlusion_emissive(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    // A black occlusion map removes all indirect light, so only the emissive term is left. The emissive map is white, and its factor is
    // (0.5, 0.25, 0), so the output is that color encoded as sRGB: (188, 137, 0).
    ImageRgba8 black = solid_image(1, 1, 0, 0, 0, 255);
    ImageRgba8 white = solid_image(1, 1, 255, 255, 255, 255);
    MaterialTextures textures;
    textures.occlusion = scene.texture(black);
    textures.emissive = scene.texture(white);
    MaterialParams params;
    params.emissive = glm::vec3(0.5f, 0.25f, 0.0f);
    params.occlusion_strength = 1.0f;
    scene.place(mesh, scene.material(params, textures), glm::vec3(0.0f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    MaterialImages images;
    images.occlusion = &black;
    images.emissive = &white;
    QuadSurface quad;
    quad.center = glm::vec3(0.0f, 0.0f, -5.0f);
    quad.half = 1.5f;
    const auto input = quad_input(snapshot.view_projection(), quad, 32, 32, kSize, kSize, glm::vec4(1.0f), true);
    check_pixel(scene.renderer().image(), 32, 32, expected_pixel(scene.renderer().environment(), input, params, images), 1);
    const std::size_t center = scene.renderer().image().index(32, 32);
    CHECK(std::abs(static_cast<int>(scene.renderer().image().color[center][0]) - 188) <= 2);
    CHECK(std::abs(static_cast<int>(scene.renderer().image().color[center][1]) - 137) <= 2);
    CHECK(scene.renderer().image().color[center][2] == 0);
}

void run_normal_map(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    // A normal texel of (255, 128, 128) decodes to a tangent-space normal along +X, so the lit surface tilts toward +X. The tangent frame
    // comes from the UVs, so the expected tangent is +X and the bitangent is -Y (V grows downward).
    ImageRgba8 normal = solid_image(1, 1, 255, 128, 128, 255);
    MaterialTextures textures;
    textures.normal = scene.texture(normal);
    MaterialParams params;
    params.roughness = 0.5f;
    scene.place(mesh, scene.material(params, textures), glm::vec3(0.0f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    MaterialImages images;
    images.normal = &normal;
    QuadSurface quad;
    quad.center = glm::vec3(0.0f, 0.0f, -5.0f);
    quad.half = 1.5f;
    const auto input = quad_input(snapshot.view_projection(), quad, 32, 32, kSize, kSize, glm::vec4(1.0f), true);
    check_pixel(scene.renderer().image(), 32, 32, expected_pixel(scene.renderer().environment(), input, params, images), 1);
    // The tilt must change the output. A flat normal map on the same quad gives a different color at this pixel.
    const std::size_t center = scene.renderer().image().index(32, 32);
    const auto tilted = scene.renderer().image().color[center];
    const auto flat_input = quad_input(snapshot.view_projection(), quad, 32, 32, kSize, kSize, glm::vec4(1.0f), true);
    const PixelReference flat = expected_pixel(scene.renderer().environment(), flat_input, params, {});
    CHECK((std::abs(static_cast<int>(tilted[0]) - flat.rgba[0]) > 4 || std::abs(static_cast<int>(tilted[1]) - flat.rgba[1]) > 4 ||
           std::abs(static_cast<int>(tilted[2]) - flat.rgba[2]) > 4));
}

void run_material_instances(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    MaterialParams parent;
    parent.base_color = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    parent.roughness = 0.3f;
    const std::uint32_t parent_id = scene.material(parent);
    MaterialOverrides overrides;
    overrides.base_color = glm::vec4(0.2f, 0.6f, 0.9f, 1.0f);
    overrides.roughness = 0.8f;
    const std::uint32_t instance_id = take(scene.renderer().create_material_instance(parent_id, overrides));
    scene.place(mesh, parent_id, glm::vec3(-1.5f, 0.0f, -5.0f));
    scene.place(mesh, instance_id, glm::vec3(1.5f, 0.0f, -5.0f));
    const FrameSnapshot snapshot = scene.draw();

    // The instance has the parent's textures and the overridden values. The parent keeps its own values.
    const MaterialParams resolved = resolve(parent, overrides);
    CHECK(scene.renderer().material_params(instance_id).roughness == resolved.roughness);
    CHECK(scene.renderer().material_params(parent_id).roughness == parent.roughness);

    QuadSurface left;
    left.center = glm::vec3(-1.5f, 0.0f, -5.0f);
    left.half = 1.5f;
    QuadSurface right = left;
    right.center.x = 1.5f;
    check_every_pixel(scene.renderer().image(), [&](std::uint32_t x, std::uint32_t y) {
        const bool on_left = x < 32;
        const QuadSurface& quad = on_left ? left : right;
        if (!engine::render::tests::covers(snapshot.view_projection(), quad, x, y, kSize, kSize)) {
            return std::make_pair(PixelReference{true, {}}, 0u);
        }
        const auto input = quad_input(snapshot.view_projection(), quad, x, y, kSize, kSize, glm::vec4(1.0f), true);
        return std::make_pair(expected_pixel(scene.renderer().environment(), input, on_left ? parent : resolved, {}), on_left ? 1u : 2u);
    });
}

void run_environment_change(Backend backend) {
    Harness scene(backend);
    const std::uint32_t mesh = scene.upload(quad_mesh(1.5f, false));
    MaterialParams params;
    params.roughness = 0.2f;
    params.metallic = 1.0f;
    params.base_color = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    scene.place(mesh, scene.material(params), glm::vec3(0.0f, 0.0f, -5.0f));
    static_cast<void>(scene.draw());

    // A new environment replaces the old one on the next frame, and the lit pixel matches the reference for the new environment.
    require_ok(scene.renderer().set_environment(uniform_radiance(0.5f)));
    const FrameSnapshot snapshot = scene.draw();
    QuadSurface quad;
    quad.center = glm::vec3(0.0f, 0.0f, -5.0f);
    quad.half = 1.5f;
    const auto input = quad_input(snapshot.view_projection(), quad, 32, 32, kSize, kSize, glm::vec4(1.0f), true);
    check_pixel(scene.renderer().image(), 32, 32, expected_pixel(scene.renderer().environment(), input, params, {}), 1);
}

}  // namespace

TEST_CASE("DX12 PBR: the metallic and roughness extremes match the CPU reference", "[gpu][dx12]") {
    run_metal_rough_extremes(Backend::Dx12);
}
TEST_CASE("Vulkan PBR: the metallic and roughness extremes match the CPU reference", "[gpu][vulkan]") {
    run_metal_rough_extremes(Backend::Vulkan);
}
TEST_CASE("DX12 PBR: sRGB base color decodes before lighting and encodes on output", "[gpu][dx12]") {
    run_color_space(Backend::Dx12);
}
TEST_CASE("Vulkan PBR: sRGB base color decodes before lighting and encodes on output", "[gpu][vulkan]") {
    run_color_space(Backend::Vulkan);
}
TEST_CASE("DX12 alpha mask: texels below the cutoff leave no pick, color, or depth", "[gpu][dx12]") {
    run_alpha_mask_edges(Backend::Dx12);
}
TEST_CASE("Vulkan alpha mask: texels below the cutoff leave no pick, color, or depth", "[gpu][vulkan]") {
    run_alpha_mask_edges(Backend::Vulkan);
}
TEST_CASE("DX12 alpha blend: layers composite back to front, whatever the submission order", "[gpu][dx12]") {
    run_alpha_blend_order(Backend::Dx12, true);
    run_alpha_blend_order(Backend::Dx12, false);
}
TEST_CASE("Vulkan alpha blend: layers composite back to front, whatever the submission order", "[gpu][vulkan]") {
    run_alpha_blend_order(Backend::Vulkan, true);
    run_alpha_blend_order(Backend::Vulkan, false);
}
TEST_CASE("DX12 two-sided: one-sided quads cull their back faces and two-sided quads draw them", "[gpu][dx12]") {
    run_two_sided(Backend::Dx12);
}
TEST_CASE("Vulkan two-sided: one-sided quads cull their back faces and two-sided quads draw them", "[gpu][vulkan]") {
    run_two_sided(Backend::Vulkan);
}
TEST_CASE("DX12 occlusion and emissive maps: a black occlusion map leaves only emissive light", "[gpu][dx12]") {
    run_occlusion_emissive(Backend::Dx12);
}
TEST_CASE("Vulkan occlusion and emissive maps: a black occlusion map leaves only emissive light", "[gpu][vulkan]") {
    run_occlusion_emissive(Backend::Vulkan);
}
TEST_CASE("DX12 normal map: the tangent-space normal tilts the lit surface", "[gpu][dx12]") {
    run_normal_map(Backend::Dx12);
}
TEST_CASE("Vulkan normal map: the tangent-space normal tilts the lit surface", "[gpu][vulkan]") {
    run_normal_map(Backend::Vulkan);
}
TEST_CASE("DX12 material instances: overrides apply, and the parent keeps its values", "[gpu][dx12]") {
    run_material_instances(Backend::Dx12);
}
TEST_CASE("Vulkan material instances: overrides apply, and the parent keeps its values", "[gpu][vulkan]") {
    run_material_instances(Backend::Vulkan);
}
TEST_CASE("DX12 environment: a new environment replaces the old one on the next frame", "[gpu][dx12]") {
    run_environment_change(Backend::Dx12);
}
TEST_CASE("Vulkan environment: a new environment replaces the old one on the next frame", "[gpu][vulkan]") {
    run_environment_change(Backend::Vulkan);
}
