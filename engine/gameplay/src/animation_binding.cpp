#include "engine/gameplay/animation_binding.hpp"

#include "engine/animation/state_graph.hpp"
#include "engine/gameplay/components.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <variant>

namespace engine::gameplay {

namespace {

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

animation::JointPose pose_from_matrix(const glm::mat4& m) {
    animation::JointPose pose;
    pose.translation = glm::vec3(m[3]);
    glm::vec3 axes[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    pose.scale = glm::vec3(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
    for (glm::vec3& axis : axes) {
        const float length = glm::length(axis);
        if (length > 0.0f) {
            axis /= length;
        }
    }
    pose.rotation = glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
    return pose;
}

}  // namespace

std::optional<ModelAnimation> animation_from_payload(const converter::ModelPayload& payload) {
    if (payload.joints.empty()) {
        return std::nullopt;
    }
    std::vector<glm::mat4> world(payload.nodes.size(), glm::mat4(1.0f));
    for (std::size_t n = 0; n < payload.nodes.size(); ++n) {
        const converter::ModelNode& node = payload.nodes[n];
        world[n] = node.parent < 0 ? node.local : world[static_cast<std::size_t>(node.parent)] * node.local;  // parents come first
    }

    ModelAnimation out;
    const std::size_t joint_count = payload.joints.size();
    out.skeleton.names.resize(joint_count);
    out.skeleton.parents.resize(joint_count);
    out.skeleton.bind_pose.resize(joint_count);
    out.skeleton.inverse_bind.resize(joint_count);
    for (std::size_t j = 0; j < joint_count; ++j) {
        const converter::ModelJoint& joint = payload.joints[j];
        out.skeleton.names[j] = joint.name;
        out.skeleton.parents[j] = joint.parent;
        const auto node = static_cast<std::size_t>(std::max(joint.node, 0));
        out.skeleton.bind_pose[j] = pose_from_matrix(joint.parent < 0 ? world[node] : payload.nodes[node].local);
        out.skeleton.inverse_bind[j] = joint.inverse_bind;
    }
    for (const converter::ModelClip& source : payload.clips) {
        animation::AnimationClip clip;
        clip.name = source.name;
        clip.duration = source.duration;
        clip.looping = true;
        clip.tracks.resize(joint_count);
        for (const converter::ModelChannel& channel : source.channels) {
            animation::JointTrack& track = clip.tracks[channel.joint];
            for (const auto& key : channel.translation) {
                track.translation.push_back({key.time, key.value});
            }
            for (const auto& key : channel.rotation) {
                track.rotation.push_back({key.time, glm::normalize(glm::quat(key.value.w, key.value.x, key.value.y, key.value.z))});
            }
            for (const auto& key : channel.scale) {
                track.scale.push_back({key.time, key.value});
            }
        }
        out.clips.emplace(clip.name, std::move(clip));
    }
    return out;
}

AnimatorBinding bind_animators(const scene::Scene& scene, const AssetReader& reader, animation::AnimatorSet& animators) {
    AnimatorBinding binding;
    for (const core::Guid& guid : scene.entity_guids()) {
        if (!has_component(scene, guid, kAnimatorId)) {
            continue;
        }
        std::string name = read<std::string>(scene, guid, kAnimatorId, "name", std::string());
        if (name.empty()) {
            name = read<std::string>(scene, guid, kNameId, "name", std::string());
        }
        if (name.empty()) {
            name = guid.to_string();
        }
        const auto problem = [&](const std::string& text) { binding.problems.push_back("animator " + name + ": " + text); };

        const core::Guid model_asset = read<core::Guid>(scene, guid, kAnimatorId, "model", core::Guid{});
        const std::optional<std::string> model_bytes = reader && !model_asset.is_nil() ? reader(model_asset) : std::nullopt;
        if (!model_bytes) {
            problem("its model is not in the packs");
            continue;
        }
        auto payload = converter::read_model_payload(*model_bytes);
        if (!payload.ok()) {
            problem("its model cannot be read: " + payload.error());
            continue;
        }
        auto animation = animation_from_payload(payload.value());
        if (!animation || animation->clips.empty()) {
            problem("its model has no skeleton and clips");
            continue;
        }

        const core::Guid graph_asset = read<core::Guid>(scene, guid, kAnimatorId, "graph", core::Guid{});
        std::string graph_text;
        if (const std::optional<std::string> text = reader && !graph_asset.is_nil() ? reader(graph_asset) : std::nullopt) {
            graph_text = *text;
        } else {
            graph_text = "{\"format\":\"engine.animation_graph\",\"version\":1,\"parameters\":[],\"default_state\":\"play\","
                         "\"states\":[{\"name\":\"play\",\"clip\":\"" +
                         animation->clips.begin()->first + "\",\"speed\":1}],\"transitions\":[]}";
        }
        auto graph = animation::parse_state_graph(graph_text);
        if (!graph.ok()) {
            problem(graph.error());
            continue;
        }
        auto machine = animation::StateMachine::create(graph.value(), animation->skeleton, animation->clips);
        if (!machine.ok()) {
            problem(machine.error());
            continue;
        }
        if (!animators.add(name, std::move(machine).value())) {
            problem("another animator already has this name");
            continue;
        }
        binding.names[guid] = name;
    }
    return binding;
}

}  // namespace engine::gameplay
