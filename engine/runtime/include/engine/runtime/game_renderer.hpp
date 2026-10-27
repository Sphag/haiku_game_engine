#pragma once

// GameRenderer: draws a loaded scene through the engine's SceneRenderer. It reads the built-in components:
//   engine.camera           the primary camera is the view
//   engine.light            directional, point, and spot lights, with shadows when the component asks
//   engine.mesh_renderer    the mesh asset's cooked model (static, or skinned and animated), its materials, the component tint on top;
//                           an asset the reader cannot supply draws a primitive (a sphere for a sphere body, a box otherwise)
//   engine.animator         a state machine over the model's skeleton and clips, reachable from scripts by the animator's name
//   engine.terrain          the cooked .terrain asset, drawn tile by tile with its splat weights
//   engine.particle_emitter a pool of small boxes simulated on the CPU
//   engine.environment      the sky color is the background
// The image comes back to the CPU (SceneRenderer reads its targets back), and present() copies it into a swapchain back buffer, so the same
// code puts the picture on screen on DX12 and Vulkan without a blit shader.

#include "engine/animation/state_machine.hpp"
#include "engine/core/guid.hpp"
#include "engine/runtime/animation_binding.hpp"
#include "engine/core/result.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/rhi/device.hpp"
#include "engine/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace engine::runtime {

struct RenderStats {
    std::uint32_t items = 0;       // drawn
    std::uint32_t lights = 0;
    std::uint32_t models = 0;      // distinct cooked models in use
    std::uint32_t skinned = 0;     // skinned items drawn
    std::uint32_t terrain_tiles = 0;
    std::uint32_t particles = 0;   // alive
    bool camera = false;           // a primary camera was found
};

class GameRenderer {
public:
    // `width` must be a multiple of 64 (the readback rows stay tight). Shaders are read from `shader_dir` as scene_*.dxil or scene_*.spv.
    [[nodiscard]] static core::Result<std::unique_ptr<GameRenderer>, std::string> create(rhi::Device& device, rhi::Backend backend,
                                                                                       const std::string& shader_dir, std::uint32_t width,
                                                                                       std::uint32_t height, AssetReader reader = {});
    ~GameRenderer();
    GameRenderer(const GameRenderer&) = delete;
    GameRenderer& operator=(const GameRenderer&) = delete;

    // Reads the scene's assets once: uploads terrain and creates the pool of every particle emitter. `animator_names` says which entity
    // owns which machine of the animator set (bind_animators returns it), so a skinned mesh finds its pose. Problems are returned as
    // text and do not stop the others.
    [[nodiscard]] std::vector<std::string> bind_scene(scene::Scene& scene, const std::map<core::Guid, std::string>& animator_names);

    // Renders the scene from its primary camera. `dt` advances the particles. A scene with no camera draws from a default view.
    [[nodiscard]] core::Result<RenderStats, std::string> render(scene::Scene& scene, animation::AnimatorSet* animators = nullptr,
                                                               double dt = 1.0 / 60.0);

    [[nodiscard]] const render::SceneImage& image() const;

    // Copies the last image into `back_buffer` (in the Present state), at the top-left, with the sky behind it, and leaves it in Present.
    // The swapchain format must be B8G8R8A8Unorm. The call waits for the copy, so the staging buffer can be reused.
    [[nodiscard]] core::Result<core::Unit, std::string> present(rhi::TextureHandle back_buffer, std::uint32_t buffer_width,
                                                              std::uint32_t buffer_height);

private:
    GameRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::runtime
