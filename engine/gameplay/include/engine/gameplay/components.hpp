#pragma once

// The engine's built-in components. They are plain data that the scene format, the editor inspector, and the runtime all read through the
// reflection catalog, so a scene authored in the editor loads in the standalone runtime without either side knowing the types. Systems that
// act on them (scripts, physics, audio, animation, rendering) read them from the scene registry. Game code can add its own components to the
// same catalog.
//
// Field conventions: a Guid field names an asset (nil means none), colors are linear RGB in 0..1, angles are degrees, lengths are meters.

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/scene/reflection.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::gameplay {

// Type IDs, one per component. Scenes name components by these strings.
inline constexpr const char* kNameId = "engine.name_tag";
inline constexpr const char* kMeshRendererId = "engine.mesh_renderer";
inline constexpr const char* kCameraId = "engine.camera";
inline constexpr const char* kLightId = "engine.light";
inline constexpr const char* kRigidBodyId = "engine.rigid_body";
inline constexpr const char* kCharacterId = "engine.character_controller";
inline constexpr const char* kScriptId = "engine.script";
inline constexpr const char* kAudioEmitterId = "engine.audio_emitter";
inline constexpr const char* kAnimatorId = "engine.animator";
inline constexpr const char* kParticleEmitterId = "engine.particle_emitter";
inline constexpr const char* kTerrainId = "engine.terrain";
inline constexpr const char* kEnvironmentId = "engine.environment";
inline constexpr const char* kUiCanvasId = "engine.ui_canvas";

// The entity's name, shown in the hierarchy and used to find it, and a group number a game can use for entities of one kind (0 means none).
struct NameTag {
    std::string name;
    std::uint32_t tag = 0;
};

struct MeshRenderer {
    core::Guid mesh;      // a model asset
    core::Guid material;  // a material asset
    bool visible = true;
    bool cast_shadows = true;
    bool receive_shadows = true;
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
};

struct Camera {
    float vertical_fov_degrees = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    bool primary = true;  // the camera the game renders from; the first primary camera wins
};

enum class LightKind : std::int32_t { Directional = 0, Point = 1, Spot = 2 };

struct Light {
    std::int32_t kind = static_cast<std::int32_t>(LightKind::Directional);
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 20.0f;               // point and spot reach, meters
    float cone_angle_degrees = 45.0f;  // spot outer angle
    bool cast_shadows = true;
};

enum class BodyMotion : std::int32_t { Static = 0, Kinematic = 1, Dynamic = 2 };
enum class BodyShape : std::int32_t { Box = 0, Sphere = 1, Capsule = 2 };

struct RigidBody {
    std::int32_t motion = static_cast<std::int32_t>(BodyMotion::Dynamic);
    std::int32_t shape = static_cast<std::int32_t>(BodyShape::Box);
    std::array<float, 3> size{1.0f, 1.0f, 1.0f};  // box edge lengths; sphere uses x as the diameter; capsule x diameter, y total height
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    bool trigger = false;
};

struct CharacterController {
    float height = 1.8f;
    float radius = 0.35f;
    float move_speed = 4.0f;
    float jump_speed = 5.0f;
};

struct Script {
    core::Guid script;  // a Lua source asset
    bool enabled = true;
};

struct AudioEmitter {
    core::Guid clip;  // an audio asset
    float volume = 1.0f;
    bool looping = false;
    bool spatial = true;
    bool play_on_start = true;
};

struct Animator {
    std::string name;  // the machine's name for scripts: animation.set_float(name, ...)
    core::Guid graph;  // an animation state graph asset
    core::Guid model;  // the model asset that carries the skeleton and clips
    float speed = 1.0f;
};

struct ParticleEmitter {
    float rate = 50.0f;     // particles per second
    float lifetime = 2.0f;  // seconds
    float speed = 2.0f;     // meters per second
    float size = 0.1f;      // meters
    std::array<float, 4> color{1.0f, 0.8f, 0.4f, 1.0f};
    std::uint32_t max_particles = 2048;
};

struct Terrain {
    core::Guid heightmap;  // a texture or blob asset holding the heights
    core::Guid material;
    std::array<float, 2> size_meters{256.0f, 256.0f};
    float height_scale = 40.0f;
};

struct Environment {
    std::array<float, 3> sky_color{0.45f, 0.62f, 0.85f};
    std::array<float, 3> ambient_color{0.25f, 0.27f, 0.32f};
    float fog_density = 0.0f;
    core::Guid gi_probes;  // a baked probe asset
    bool ray_traced_shadows = false;
    bool ray_traced_reflections = false;
};

struct UiCanvas {
    core::Guid layout;  // a UI layout asset
    bool visible = true;
};

// Registers every built-in component. A catalog that already holds one of them reports the duplicate for it.
[[nodiscard]] core::Result<core::Unit, std::vector<scene::ReflectionError>> register_builtin_components(scene::ComponentCatalog& catalog);

}  // namespace engine::gameplay
