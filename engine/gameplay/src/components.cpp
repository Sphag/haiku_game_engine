#include "engine/gameplay/components.hpp"

#include <utility>

namespace engine::gameplay {

namespace {

using scene::ComponentBuilder;
using scene::Range;

// Builds one descriptor and adds it, collecting every problem so a bad registration names all of them.
template <typename C>
void add(scene::ComponentCatalog& catalog, const ComponentBuilder<C>& builder, std::vector<scene::ReflectionError>& errors) {
    auto built = builder.build();
    if (!built.ok()) {
        for (auto& error : built.error()) {
            errors.push_back(std::move(error));
        }
        return;
    }
    auto added = catalog.add(std::move(built).value());
    if (!added.ok()) {
        for (auto& error : added.error()) {
            errors.push_back(std::move(error));
        }
    }
}

const Range kUnit{0.0, 1.0};
const Range kPositive{0.0, 1.0e6};

}  // namespace

core::Result<core::Unit, std::vector<scene::ReflectionError>> register_builtin_components(scene::ComponentCatalog& catalog) {
    std::vector<scene::ReflectionError> errors;

    ComponentBuilder<NameTag> name(kNameId, 1);
    name.field<&NameTag::name>("name", std::string());
    name.field<&NameTag::tag>("tag", 0u);
    add(catalog, name, errors);

    ComponentBuilder<MeshRenderer> mesh(kMeshRendererId, 1);
    mesh.field<&MeshRenderer::mesh>("mesh", core::Guid{});
    mesh.field<&MeshRenderer::material>("material", core::Guid{});
    mesh.field<&MeshRenderer::visible>("visible", true);
    mesh.field<&MeshRenderer::cast_shadows>("cast_shadows", true);
    mesh.field<&MeshRenderer::receive_shadows>("receive_shadows", true);
    mesh.field<&MeshRenderer::tint>("tint", std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
    add(catalog, mesh, errors);

    ComponentBuilder<Camera> camera(kCameraId, 1);
    camera.field<&Camera::vertical_fov_degrees>("vertical_fov_degrees", 60.0f, Range{1.0, 170.0});
    camera.field<&Camera::near_plane>("near_plane", 0.1f, Range{0.001, 1000.0});
    camera.field<&Camera::far_plane>("far_plane", 1000.0f, Range{0.01, 1.0e7});
    camera.field<&Camera::primary>("primary", true);
    add(catalog, camera, errors);

    ComponentBuilder<Light> light(kLightId, 1);
    light.field<&Light::kind>("kind", 0, Range{0.0, 2.0});
    light.field<&Light::color>("color", std::array<float, 3>{1.0f, 1.0f, 1.0f});
    light.field<&Light::intensity>("intensity", 1.0f, Range{0.0, 1.0e6});
    light.field<&Light::range>("range", 20.0f, Range{0.01, 1.0e5});
    light.field<&Light::cone_angle_degrees>("cone_angle_degrees", 45.0f, Range{1.0, 179.0});
    light.field<&Light::cast_shadows>("cast_shadows", true);
    add(catalog, light, errors);

    ComponentBuilder<RigidBody> body(kRigidBodyId, 1);
    body.field<&RigidBody::motion>("motion", 2, Range{0.0, 2.0});
    body.field<&RigidBody::shape>("shape", 0, Range{0.0, 2.0});
    body.field<&RigidBody::size>("size", std::array<float, 3>{1.0f, 1.0f, 1.0f});
    body.field<&RigidBody::mass>("mass", 1.0f, Range{0.001, 1.0e6});
    body.field<&RigidBody::friction>("friction", 0.5f, Range{0.0, 10.0});
    body.field<&RigidBody::restitution>("restitution", 0.0f, kUnit);
    body.field<&RigidBody::trigger>("trigger", false);
    add(catalog, body, errors);

    ComponentBuilder<CharacterController> character(kCharacterId, 1);
    character.field<&CharacterController::height>("height", 1.8f, Range{0.2, 10.0});
    character.field<&CharacterController::radius>("radius", 0.35f, Range{0.05, 5.0});
    character.field<&CharacterController::move_speed>("move_speed", 4.0f, kPositive);
    character.field<&CharacterController::jump_speed>("jump_speed", 5.0f, kPositive);
    add(catalog, character, errors);

    ComponentBuilder<Script> script(kScriptId, 1);
    script.field<&Script::script>("script", core::Guid{});
    script.field<&Script::enabled>("enabled", true);
    add(catalog, script, errors);

    ComponentBuilder<AudioEmitter> audio(kAudioEmitterId, 1);
    audio.field<&AudioEmitter::clip>("clip", core::Guid{});
    audio.field<&AudioEmitter::volume>("volume", 1.0f, kUnit);
    audio.field<&AudioEmitter::looping>("looping", false);
    audio.field<&AudioEmitter::spatial>("spatial", true);
    audio.field<&AudioEmitter::play_on_start>("play_on_start", true);
    add(catalog, audio, errors);

    ComponentBuilder<Animator> animator(kAnimatorId, 1);
    animator.field<&Animator::name>("name", std::string());
    animator.field<&Animator::graph>("graph", core::Guid{});
    animator.field<&Animator::model>("model", core::Guid{});
    animator.field<&Animator::speed>("speed", 1.0f, Range{0.0, 100.0});
    add(catalog, animator, errors);

    ComponentBuilder<ParticleEmitter> particles(kParticleEmitterId, 1);
    particles.field<&ParticleEmitter::rate>("rate", 50.0f, Range{0.0, 1.0e6});
    particles.field<&ParticleEmitter::lifetime>("lifetime", 2.0f, Range{0.01, 600.0});
    particles.field<&ParticleEmitter::speed>("speed", 2.0f, Range{0.0, 1.0e4});
    particles.field<&ParticleEmitter::size>("size", 0.1f, Range{0.0001, 100.0});
    particles.field<&ParticleEmitter::color>("color", std::array<float, 4>{1.0f, 0.8f, 0.4f, 1.0f});
    particles.field<&ParticleEmitter::max_particles>("max_particles", 2048u, Range{1.0, 1.0e7});
    add(catalog, particles, errors);

    ComponentBuilder<Terrain> terrain(kTerrainId, 1);
    terrain.field<&Terrain::heightmap>("heightmap", core::Guid{});
    terrain.field<&Terrain::material>("material", core::Guid{});
    terrain.field<&Terrain::size_meters>("size_meters", std::array<float, 2>{256.0f, 256.0f});
    terrain.field<&Terrain::height_scale>("height_scale", 40.0f, Range{0.0, 1.0e5});
    add(catalog, terrain, errors);

    ComponentBuilder<Environment> environment(kEnvironmentId, 1);
    environment.field<&Environment::sky_color>("sky_color", std::array<float, 3>{0.45f, 0.62f, 0.85f});
    environment.field<&Environment::ambient_color>("ambient_color", std::array<float, 3>{0.25f, 0.27f, 0.32f});
    environment.field<&Environment::fog_density>("fog_density", 0.0f, Range{0.0, 10.0});
    environment.field<&Environment::gi_probes>("gi_probes", core::Guid{});
    environment.field<&Environment::ray_traced_shadows>("ray_traced_shadows", false);
    environment.field<&Environment::ray_traced_reflections>("ray_traced_reflections", false);
    add(catalog, environment, errors);

    ComponentBuilder<UiCanvas> canvas(kUiCanvasId, 1);
    canvas.field<&UiCanvas::layout>("layout", core::Guid{});
    canvas.field<&UiCanvas::visible>("visible", true);
    add(catalog, canvas, errors);

    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }
    return core::success();
}

}  // namespace engine::gameplay
