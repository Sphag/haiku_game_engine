#pragma once

// GameRenderer: draws a loaded scene through the engine's SceneRenderer. It reads the built-in components: the primary camera sets the view,
// light components become lights (directional, point, spot), and every visible mesh renderer becomes a drawn item with its tint. The image
// comes back to the CPU (SceneRenderer reads its targets back), and present() copies it into a swapchain back buffer, so the same code
// puts the picture on screen on DX12 and Vulkan without a blit shader.
//
// Meshes: the cooked model payload is decoded by the converter, which the runtime must not link, so a mesh renderer draws a built-in
// primitive: a sphere when its entity has a sphere rigid body, a box otherwise. A MeshProvider (below) is the hook for real geometry.

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/rhi/device.hpp"
#include "engine/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace engine::render {
class SceneRenderer;
}

namespace engine::runtime {

// Geometry for a mesh asset, in meters. Return nothing to draw the primitive instead.
using MeshProvider = std::function<std::optional<render::MeshData>(const core::Guid&)>;

struct RenderStats {
    std::uint32_t items = 0;   // drawn
    std::uint32_t lights = 0;
    bool camera = false;       // a primary camera was found
};

class GameRenderer {
public:
    // `width` must be a multiple of 64 (the readback rows stay tight). Shaders are read from `shader_dir` as scene_*.dxil or scene_*.spv.
    [[nodiscard]] static core::Result<std::unique_ptr<GameRenderer>, std::string> create(rhi::Device& device, rhi::Backend backend,
                                                                                       const std::string& shader_dir, std::uint32_t width,
                                                                                       std::uint32_t height);
    ~GameRenderer();
    GameRenderer(const GameRenderer&) = delete;
    GameRenderer& operator=(const GameRenderer&) = delete;

    // Renders the scene from its primary camera. A scene with no camera draws from a default view looking at the origin.
    [[nodiscard]] core::Result<RenderStats, std::string> render(scene::Scene& scene);

    [[nodiscard]] const render::SceneImage& image() const;

    // Copies the last image into `back_buffer` (in the Present state), letterboxed at the top-left, black elsewhere, and leaves it in
    // Present. The swapchain format must be B8G8R8A8Unorm. The call waits for the copy, so the staging buffer can be reused.
    [[nodiscard]] core::Result<core::Unit, std::string> present(rhi::TextureHandle back_buffer, std::uint32_t buffer_width,
                                                              std::uint32_t buffer_height);

private:
    GameRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::runtime
