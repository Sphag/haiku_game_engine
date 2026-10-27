#include "engine/runtime/game_renderer.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/gameplay/components.hpp"
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/scene_renderer.hpp"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <variant>
#include <vector>

namespace engine::runtime {

namespace {

constexpr double kPi = 3.14159265358979323846;

template <typename T>
T read(const scene::Scene& scene, const core::Guid& entity, const char* type, const char* field, T fallback) {
    const auto value = scene.field(entity, type, field);
    if (value && std::holds_alternative<T>(*value)) {
        return std::get<T>(*value);
    }
    return fallback;
}

bool has_component(const scene::Scene& scene, const core::Guid& entity, const char* type) {
    const auto types = scene.component_types(entity);
    return std::find(types.begin(), types.end(), type) != types.end();
}

std::string describe(const rhi::RhiError& error) {
    return error.message;
}

std::vector<std::uint8_t> load_shader(rhi::Backend backend, const std::string& dir, const std::string& name, std::string& problem) {
    const std::string file = dir + "/" + name + (backend == rhi::Backend::Dx12 ? ".dxil" : ".spv");
    auto path = core::Path::from_utf8(file);
    if (!path.ok()) {
        problem = "shader path is not valid UTF-8: " + file;
        return {};
    }
    auto bytes = core::read_file(path.value());
    if (!bytes.ok()) {
        problem = "cannot read the shader " + file;
        return {};
    }
    return std::vector<std::uint8_t>(bytes.value().begin(), bytes.value().end());
}

}  // namespace

struct GameRenderer::Impl {
    rhi::Device* device = nullptr;
    std::unique_ptr<render::SceneRenderer> renderer;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t box_mesh = 0;
    std::uint32_t sphere_mesh = 0;
    rhi::BufferHandle staging;
    std::uint64_t staging_bytes = 0;
    rhi::FenceHandle fence;
    std::uint64_t fence_value = 0;
    std::array<std::uint8_t, 3> sky{115, 158, 217};  // BGR order is applied when presenting; this is the environment's sky_color in RGB 0..255
};

GameRenderer::GameRenderer() : impl_(std::make_unique<Impl>()) {}

GameRenderer::~GameRenderer() {
    if (impl_ != nullptr && impl_->device != nullptr) {
        static_cast<void>(impl_->device->wait_idle());
        impl_->renderer.reset();
        if (impl_->staging.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->staging));
        }
        if (impl_->fence.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->fence));
        }
    }
}

core::Result<std::unique_ptr<GameRenderer>, std::string> GameRenderer::create(rhi::Device& device, rhi::Backend backend,
                                                                              const std::string& shader_dir, std::uint32_t width,
                                                                              std::uint32_t height) {
    if (width == 0 || height == 0 || width % 64 != 0) {
        return core::failure(std::string("the render width must be a positive multiple of 64"));
    }
    std::string problem;
    render::SceneShaders shaders;
    const auto load = [&](rhi::ShaderStage stage, const char* entry, const char* name) {
        return rhi::ShaderBinary{stage, entry, load_shader(backend, shader_dir, name, problem)};
    };
    shaders.vertex = load(rhi::ShaderStage::Vertex, "VSMain", "scene_vertex");
    shaders.pixel = load(rhi::ShaderStage::Pixel, "PSMain", "scene_pixel");
    shaders.pixel_blend = load(rhi::ShaderStage::Pixel, "PSBlend", "scene_pixel_blend");
    shaders.shadow_vertex = load(rhi::ShaderStage::Vertex, "VSShadow", "scene_shadow_vertex");
    shaders.shadow_pixel = load(rhi::ShaderStage::Pixel, "PSShadow", "scene_shadow_pixel");
    if (!problem.empty()) {
        return core::failure(problem + " (the game needs its shaders folder beside the executable)");
    }

    render::SceneRendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.draw_path = render::DrawPath::Indexed;
    desc.geometry = render::GeometryPath::Conventional;
    auto created = render::SceneRenderer::create(device, desc, shaders);
    if (!created.ok()) {
        return core::failure("cannot create the scene renderer: " + describe(created.error()));
    }

    std::unique_ptr<GameRenderer> out(new GameRenderer());
    out->impl_->device = &device;
    out->impl_->renderer = std::move(created).value();
    out->impl_->width = width;
    out->impl_->height = height;
    auto box = out->impl_->renderer->upload_mesh(render::make_box_mesh(glm::vec3(0.5f)));
    auto sphere = out->impl_->renderer->upload_mesh(render::make_sphere_mesh(0.5f, 16, 32));
    if (!box.ok() || !sphere.ok()) {
        return core::failure(std::string("cannot upload the primitive meshes"));
    }
    out->impl_->box_mesh = box.value();
    out->impl_->sphere_mesh = sphere.value();
    auto fence = device.create_fence(0);
    if (!fence.ok()) {
        return core::failure("cannot create the present fence: " + describe(fence.error()));
    }
    out->impl_->fence = std::move(fence).value();
    return core::success(std::move(out));
}

const render::SceneImage& GameRenderer::image() const {
    return impl_->renderer->image();
}

core::Result<RenderStats, std::string> GameRenderer::render(scene::Scene& scene) {
    RenderStats stats;
    render::Camera camera;
    camera.aspect = static_cast<double>(impl_->width) / static_cast<double>(impl_->height);
    camera.position = glm::dvec3(0.0, 2.0, 8.0);

    std::vector<render::Light> lights;
    std::vector<render::RenderableRef> renderables;
    std::uint32_t light_id = 1;

    for (const core::Guid& guid : scene.entity_guids()) {
        const glm::dmat4* world = scene.world_matrix(guid);
        const auto handle = scene.handle(guid);
        if (world == nullptr || !handle) {
            continue;
        }
        const glm::dvec3 position((*world)[3]);
        glm::dmat3 axes(*world);
        for (int column = 0; column < 3; ++column) {
            const double length = glm::length(axes[column]);
            if (length > 0.0) {
                axes[column] /= length;
            }
        }
        const glm::dquat rotation = glm::normalize(glm::quat_cast(axes));

        if (!stats.camera && has_component(scene, guid, gameplay::kCameraId) && read<bool>(scene, guid, gameplay::kCameraId, "primary", true)) {
            camera.position = position;
            camera.orientation = rotation;
            camera.fov_y_radians = read<float>(scene, guid, gameplay::kCameraId, "vertical_fov_degrees", 60.0f) * kPi / 180.0;
            camera.near_plane = read<float>(scene, guid, gameplay::kCameraId, "near_plane", 0.1f);
            camera.far_plane = read<float>(scene, guid, gameplay::kCameraId, "far_plane", 1000.0f);
            stats.camera = true;
        }

        if (has_component(scene, guid, gameplay::kEnvironmentId)) {
            const auto sky = read<std::array<float, 3>>(scene, guid, gameplay::kEnvironmentId, "sky_color", {0.45f, 0.62f, 0.85f});
            for (int channel = 0; channel < 3; ++channel) {
                impl_->sky[channel] = static_cast<std::uint8_t>(std::clamp(sky[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }

        if (has_component(scene, guid, gameplay::kLightId)) {
            render::Light light;
            light.id = light_id++;
            const auto kind = read<std::int32_t>(scene, guid, gameplay::kLightId, "kind", 0);
            light.type = kind == 1 ? render::LightType::Point : kind == 2 ? render::LightType::Spot : render::LightType::Directional;
            light.position = position;
            light.direction = glm::vec3(glm::normalize(rotation * glm::dvec3(0.0, 0.0, -1.0)));
            const auto color = read<std::array<float, 3>>(scene, guid, gameplay::kLightId, "color", {1.0f, 1.0f, 1.0f});
            light.color = glm::vec3(color[0], color[1], color[2]);
            light.intensity = read<float>(scene, guid, gameplay::kLightId, "intensity", 1.0f);
            light.range = read<float>(scene, guid, gameplay::kLightId, "range", 20.0f);
            const float cone = read<float>(scene, guid, gameplay::kLightId, "cone_angle_degrees", 45.0f) * static_cast<float>(kPi) / 180.0f;
            light.outer_angle = std::clamp(cone * 0.5f, 0.05f, 1.5f);
            light.inner_angle = light.outer_angle * 0.8f;
            light.casts_shadow = read<bool>(scene, guid, gameplay::kLightId, "cast_shadows", false);
            lights.push_back(light);
            ++stats.lights;
        }

        if (has_component(scene, guid, gameplay::kMeshRendererId) && read<bool>(scene, guid, gameplay::kMeshRendererId, "visible", true)) {
            render::RenderableRef item;
            item.node = *handle;
            const bool sphere = has_component(scene, guid, gameplay::kRigidBodyId) &&
                                read<std::int32_t>(scene, guid, gameplay::kRigidBodyId, "shape", 0) == 1;
            item.mesh = sphere ? impl_->sphere_mesh : impl_->box_mesh;
            const auto tint = read<std::array<float, 4>>(scene, guid, gameplay::kMeshRendererId, "tint", {1.0f, 1.0f, 1.0f, 1.0f});
            item.color = glm::vec4(tint[0], tint[1], tint[2], tint[3]);
            renderables.push_back(item);
        }
    }

    if (!camera.is_valid()) {
        return core::failure(std::string("the primary camera is not valid (check its field of view and clip planes)"));
    }
    const render::FrameSnapshot snapshot =
        render::extract_frame(scene.graph(), camera, impl_->renderer->bounds(), renderables, lights);
    if (auto drawn = impl_->renderer->render(snapshot); !drawn.ok()) {
        return core::failure("the frame could not be drawn: " + describe(drawn.error()));
    }
    stats.items = snapshot.stats().drawn;
    return core::success(stats);
}

core::Result<core::Unit, std::string> GameRenderer::present(rhi::TextureHandle back_buffer, std::uint32_t buffer_width,
                                                           std::uint32_t buffer_height) {
    const render::SceneImage& image = impl_->renderer->image();
    const std::uint64_t pitch = rhi::copy_row_pitch(buffer_width, rhi::Format::B8G8R8A8Unorm);
    const std::uint64_t bytes = pitch * (buffer_height - 1) + static_cast<std::uint64_t>(buffer_width) * 4;
    if (!impl_->staging.valid() || impl_->staging_bytes != bytes) {
        if (impl_->staging.valid()) {
            static_cast<void>(impl_->device->destroy(impl_->staging));
        }
        auto staging = impl_->device->create_buffer(
            rhi::BufferDesc{bytes, rhi::BufferUsage::CopySource | rhi::BufferUsage::CpuWrite, "game present staging"});
        if (!staging.ok()) {
            return core::failure("cannot create the present buffer: " + describe(staging.error()));
        }
        impl_->staging = std::move(staging).value();
        impl_->staging_bytes = bytes;
    }

    // The image is RGBA8 and the back buffer BGRA8, so the red and blue channels swap on the way. Pixels outside the image stay black.
    std::vector<std::uint8_t> packed(bytes, 0);
    const std::uint32_t copy_w = std::min(image.width, buffer_width);
    const std::uint32_t copy_h = std::min(image.height, buffer_height);
    for (std::uint32_t y = 0; y < copy_h; ++y) {
        std::uint8_t* row = packed.data() + y * pitch;
        for (std::uint32_t x = 0; x < copy_w; ++x) {
            const std::size_t index = image.index(x, y);
            const auto& texel = image.color[index];
            // Nothing was drawn here (no pick id): the environment's sky shows instead of the cleared black.
            const bool sky = image.pick_ids[index] == 0;
            row[x * 4 + 0] = sky ? impl_->sky[2] : texel[2];
            row[x * 4 + 1] = sky ? impl_->sky[1] : texel[1];
            row[x * 4 + 2] = sky ? impl_->sky[0] : texel[0];
            row[x * 4 + 3] = 255;
        }
    }
    if (auto uploaded = impl_->device->upload_buffer(impl_->staging, 0, packed.size(), packed.data()); !uploaded.ok()) {
        return core::failure("cannot fill the present buffer: " + describe(uploaded.error()));
    }

    auto list_result = impl_->device->create_command_list(rhi::QueueType::Graphics);
    if (!list_result.ok()) {
        return core::failure("cannot record the present: " + describe(list_result.error()));
    }
    std::unique_ptr<rhi::CommandList> list = std::move(list_result).value();
    rhi::RhiStatus recorded = list->transition(back_buffer, rhi::ResourceState::Present, rhi::ResourceState::CopyDestination);
    if (recorded.ok()) {
        recorded = list->copy_buffer_to_texture(impl_->staging, 0, back_buffer);
    }
    if (recorded.ok()) {
        recorded = list->transition(back_buffer, rhi::ResourceState::CopyDestination, rhi::ResourceState::Present);
    }
    if (recorded.ok()) {
        recorded = list->close();
    }
    if (recorded.ok()) {
        const std::vector<rhi::CommandList*> lists{list.get()};
        recorded = impl_->device->submit(rhi::QueueType::Graphics, lists, impl_->fence, ++impl_->fence_value);
    }
    if (recorded.ok()) {
        recorded = impl_->device->wait_fence(impl_->fence, impl_->fence_value);
    }
    if (!recorded.ok()) {
        return core::failure("the present copy failed: " + describe(recorded.error()));
    }
    return core::success();
}

}  // namespace engine::runtime
