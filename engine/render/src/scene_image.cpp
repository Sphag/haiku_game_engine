#include "engine/render/scene_image.hpp"

namespace engine::render {

std::optional<ecs::EntityHandle> pick_entity(const SceneImage& image, const FrameSnapshot& snapshot, std::uint32_t x, std::uint32_t y) {
    if (x >= image.width || y >= image.height || image.pick_ids.size() != static_cast<std::size_t>(image.width) * image.height) {
        return std::nullopt;
    }
    return snapshot.entity_of(image.pick_ids[image.index(x, y)]);
}

}  // namespace engine::render
