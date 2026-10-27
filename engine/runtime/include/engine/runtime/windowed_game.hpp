#pragma once

// The windowed game loop: a window, a device, a swapchain, and the frame loop that steps the GameWorld and draws the scene through
// GameRenderer. It runs until the window closes (or Escape is pressed), or for `frames` frames when a count is given.

#include "engine/gameplay/game_world.hpp"
#include "engine/runtime/runtime_options.hpp"
#include "engine/scene/scene.hpp"

#include <string>

namespace engine::runtime {

// Returns 0 when the game ran and closed normally, and 1 with the reason printed to stderr when the window, the device, or a frame failed.
[[nodiscard]] int run_windowed_game(scene::Scene& scene, gameplay::GameWorld& world, const RuntimeOptions& options,
                                    const std::string& shader_dir);

}  // namespace engine::runtime
