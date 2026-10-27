#include "engine/gameplay/game_world.hpp"

#include "engine/gameplay/components.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace engine::gameplay {

namespace {

physics::BodyMotion to_motion(std::int32_t value) {
    switch (static_cast<BodyMotion>(value)) {
        case BodyMotion::Static:
            return physics::BodyMotion::Static;
        case BodyMotion::Kinematic:
            return physics::BodyMotion::Kinematic;
        case BodyMotion::Dynamic:
            break;
    }
    return physics::BodyMotion::Dynamic;
}

// The collider the component describes, placed at the entity's transform. Sizes are final meters; the transform's scale is not applied.
physics::BodyDesc body_desc(ecs::EntityHandle entity, const RigidBody& body, const scene::Transform& transform) {
    physics::BodyDesc desc;
    desc.entity = entity;
    desc.motion = to_motion(body.motion);
    const float x = std::max(body.size[0], 0.001f);
    const float y = std::max(body.size[1], 0.001f);
    const float z = std::max(body.size[2], 0.001f);
    switch (static_cast<BodyShape>(body.shape)) {
        case BodyShape::Sphere:
            desc.shape = physics::ShapeKind::Sphere;
            desc.radius = x * 0.5f;
            break;
        case BodyShape::Capsule:
            desc.shape = physics::ShapeKind::Capsule;
            desc.radius = x * 0.5f;
            desc.half_height = std::max(0.0f, (y - x) * 0.5f);
            break;
        case BodyShape::Box:
            desc.shape = physics::ShapeKind::Box;
            desc.half_extents = glm::vec3(x, y, z) * 0.5f;
            break;
    }
    desc.position = transform.position;
    desc.rotation = glm::quat(static_cast<float>(transform.rotation.w), static_cast<float>(transform.rotation.x),
                              static_cast<float>(transform.rotation.y), static_cast<float>(transform.rotation.z));
    desc.friction = body.friction;
    desc.restitution = body.restitution;
    desc.trigger = body.trigger;
    return desc;
}

// Component data lives in the scene as reflected fields, so a system reads the fields it needs by name.
template <typename T>
T read(const scene::Scene& scene, const core::Guid& entity, const char* type, const char* field, T fallback) {
    const auto value = scene.field(entity, type, field);
    if (value && std::holds_alternative<T>(*value)) {
        return std::get<T>(*value);
    }
    return fallback;
}

RigidBody read_body(const scene::Scene& scene, const core::Guid& entity) {
    RigidBody body;
    body.motion = read<std::int32_t>(scene, entity, kRigidBodyId, "motion", body.motion);
    body.shape = read<std::int32_t>(scene, entity, kRigidBodyId, "shape", body.shape);
    body.size = read<std::array<float, 3>>(scene, entity, kRigidBodyId, "size", body.size);
    body.mass = read<float>(scene, entity, kRigidBodyId, "mass", body.mass);
    body.friction = read<float>(scene, entity, kRigidBodyId, "friction", body.friction);
    body.restitution = read<float>(scene, entity, kRigidBodyId, "restitution", body.restitution);
    body.trigger = read<bool>(scene, entity, kRigidBodyId, "trigger", body.trigger);
    return body;
}

bool has_component(const scene::Scene& scene, const core::Guid& entity, const char* type) {
    const auto types = scene.component_types(entity);
    return std::find(types.begin(), types.end(), type) != types.end();
}

}  // namespace

core::Result<SceneBindings, std::string> bind_scene(scene::Scene& scene, physics::PhysicsWorld& physics, script::ScriptRunner& runner,
                                                    const ScriptSource& read_script, std::vector<std::string>& errors) {
    SceneBindings bindings;
    for (const core::Guid& guid : scene.entity_guids()) {
        const auto handle = scene.handle(guid);
        const scene::Transform* transform = scene.transform(guid);
        if (!handle || transform == nullptr) {
            continue;
        }
        if (has_component(scene, guid, kRigidBodyId)) {
            const RigidBody body = read_body(scene, guid);
            auto added = physics.add_body(body_desc(*handle, body, *transform));
            if (!added.ok()) {
                return core::failure(std::string("a rigid body was refused by the physics world"));
            }
            if (to_motion(body.motion) == physics::BodyMotion::Dynamic) {
                bindings.dynamic_bodies.emplace_back(guid, *handle);
            }
            ++bindings.bodies;
        }
        if (has_component(scene, guid, kScriptId)) {
            const bool enabled = read<bool>(scene, guid, kScriptId, "enabled", true);
            const core::Guid source_guid = read<core::Guid>(scene, guid, kScriptId, "script", core::Guid{});
            if (!enabled || source_guid.is_nil()) {
                continue;
            }
            auto source = read_script ? read_script(source_guid)
                                      : core::Result<std::string, std::string>(core::failure(std::string("no script reader")));
            if (!source.ok()) {
                errors.push_back("script " + source_guid.to_string() + ": " + source.error());
                continue;
            }
            auto attached = runner.attach(*handle, source_guid.to_string(), source.value());
            if (!attached.ok()) {
                errors.push_back("script " + source_guid.to_string() + ": " + attached.error().message);
                continue;
            }
            bindings.scripted.push_back(guid);
        }
    }
    return core::success(std::move(bindings));
}

void sync_scene(scene::Scene& scene, physics::PhysicsWorld& physics, const SceneBindings& bindings) {
    for (const auto& [guid, entity] : bindings.dynamic_bodies) {
        const auto pose = physics.step_transform(entity);
        const scene::Transform* current = scene.transform(guid);
        if (pose && current != nullptr) {
            scene::Transform next = *current;
            next.position = pose->position;
            next.rotation = glm::dquat(pose->rotation.w, pose->rotation.x, pose->rotation.y, pose->rotation.z);
            static_cast<void>(scene.set_transform(guid, next));
        }
    }
    // A script writes its entity's transform in place. Applying it again through the scene marks the node dirty, so the world matrices
    // follow. An entity inside a prefab instance takes its transform from the prefab and is skipped by the scene, which is its rule.
    for (const core::Guid& guid : bindings.scripted) {
        if (const scene::Transform* current = scene.transform(guid)) {
            static_cast<void>(scene.set_transform(guid, *current));
        }
    }
    scene.update();
}

core::Result<std::unique_ptr<GameWorld>, std::string> GameWorld::create(scene::Scene& scene, ScriptSource read_script,
                                                                  const script::ScriptServices& extra) {
    std::unique_ptr<GameWorld> world(new GameWorld());
    world->scene_ = &scene;

    auto physics = physics::PhysicsWorld::create(physics::PhysicsWorldDesc{});
    if (!physics.ok()) {
        return core::failure(std::string("the physics world could not start"));
    }
    world->physics_ = std::move(physics.value());
    world->audio_ = audio::AudioEngine::create_offline();
    world->scripts_ = std::make_unique<script::ScriptEngine>();
    script::ScriptServices services = extra;
    services.registry = &scene.mutable_registry();
    services.physics = world->physics_.get();
    services.audio = world->audio_.get();
    world->scripts_->bind(services);
    world->runner_ = std::make_unique<script::ScriptRunner>(*world->scripts_, scene.registry());

    std::vector<std::string> bind_errors;
    auto bound = bind_scene(scene, *world->physics_, *world->runner_, read_script, bind_errors);
    if (!bound.ok()) {
        return core::failure(bound.error());
    }
    world->bindings_ = std::move(bound.value());
    world->errors_ = std::move(bind_errors);
    world->stats_.bodies = world->bindings_.bodies;
    return core::success(std::move(world));
}

GameWorld::~GameWorld() {
    // Scripts stop first so their destroy callbacks still see live bodies; the scene's registry outlives this object.
    if (runner_ != nullptr) {
        runner_->stop_all();
    }
    if (audio_ != nullptr) {
        audio_->stop_all();
    }
    if (physics_ != nullptr) {
        physics_->close();
    }
}

void GameWorld::advance(double frame_seconds) {
    if (!(frame_seconds > 0.0)) {
        return;
    }
    runner_->begin_frame();
    accumulator_ += frame_seconds;
    std::uint32_t steps = 0;
    while (accumulator_ >= physics::kFixedStep && steps < physics::kDefaultMaxCatchUpSteps) {
        accumulator_ -= physics::kFixedStep;
        if (!physics_->update(physics::kFixedStep).ok()) {
            errors_.emplace_back("physics: the world is closed");
        }
        runner_->fixed_update(physics::kFixedStep);
        ++stats_.fixed_steps;
        ++steps;
    }
    if (accumulator_ >= physics::kFixedStep) {
        accumulator_ = 0.0;  // the catch-up limit was reached; the rest of the frame's time is dropped, as physics drops it
    }
    runner_->update(frame_seconds);

    sync_scene(*scene_, *physics_, bindings_);
    stats_.seconds += frame_seconds;
}

GameWorldStats GameWorld::stats() const {
    GameWorldStats out = stats_;
    out.scripts = runner_->instance_count();
    return out;
}

std::vector<std::string> GameWorld::take_errors() {
    std::vector<std::string> out;
    out.swap(errors_);
    for (auto& error : runner_->take_errors()) {
        out.push_back(std::move(error.message));
    }
    for (auto& error : scripts_->take_errors()) {
        out.push_back(std::move(error.message));
    }
    return out;
}

}  // namespace engine::gameplay
