#pragma once

// Animators: turns a model's skeleton and clips, plus an animation graph, into the named state machines that scripts steer through the Lua
// animation module. It needs no device, so the headless runtime and the windowed one bind the same animators.

#include "asset_converter/model_format.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/core/guid.hpp"
#include "engine/scene/scene.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine::runtime {

// The importer payload of an asset (a model's OWMD bytes, a terrain's OWTR bytes, a graph's JSON text), or nothing when it is not available.
using AssetReader = std::function<std::optional<std::string>(const core::Guid&)>;

struct ModelAnimation {
    animation::Skeleton skeleton;
    animation::ClipLibrary clips;
};

// The skeleton and clips of a payload. Nothing for a model without joints. A root joint takes its node's world matrix, so the transforms of
// the non-joint ancestors (an exporter's armature node, the unit scale) are kept.
[[nodiscard]] std::optional<ModelAnimation> animation_from_payload(const converter::ModelPayload& payload);

struct AnimatorBinding {
    std::map<core::Guid, std::string> names;  // by the entity that carries the animator: the name its machine has in the set
    std::vector<std::string> problems;
};

// Builds a machine for every engine.animator component and adds it to `animators` under the component's name (the entity's name when the
// component has none). The graph is the component's graph asset; without one, the model's first clip plays on a loop.
[[nodiscard]] AnimatorBinding bind_animators(const scene::Scene& scene, const AssetReader& reader, animation::AnimatorSet& animators);

}  // namespace engine::runtime
