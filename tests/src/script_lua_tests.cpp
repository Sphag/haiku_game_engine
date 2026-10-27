// The Lua gameplay API (US-055) on the CPU: scripts drive entities, transforms, input actions, assets, physics queries, and events
// through the bindings. The tests check stack balance, type errors, Lua exceptions, and the controlled errors for destroyed entities
// and missing services. They run on every operating system with no renderer.

#include "engine/assets/asset_registry.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/platform/action_map.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_state.hpp"
#include "engine/scene/transform.hpp"
#include "engine/script/script_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using engine::assets::AssetRecord;
using engine::assets::AssetRegistry;
using engine::core::Guid;
using engine::core::Path;
using engine::ecs::EntityHandle;
using engine::ecs::Registry;
using engine::physics::BodyDesc;
using engine::physics::BodyMotion;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldDesc;
using engine::physics::ShapeKind;
using engine::platform::ActionKind;
using engine::platform::ActionMap;
using engine::platform::ActionEvaluator;
using engine::platform::Binding;
using engine::platform::InputEvent;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyPressed;
using engine::platform::KeySource;
using engine::platform::WindowId;
using engine::script::ScriptEngine;
using engine::script::ScriptError;
using engine::script::ScriptErrorKind;
using engine::script::ScriptServices;

namespace {

constexpr const char* kCrateGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";
constexpr const char* kMeshGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3302";
constexpr const char* kTextureGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3303";

// One scripted world: a registry, the input state, the asset index, and optionally a physics world, all bound to one engine.
struct Fixture {
    Registry registry;
    ActionMap actions;
    ActionEvaluator evaluator;
    InputState input;
    AssetRegistry assets;
    std::unique_ptr<PhysicsWorld> physics;
    ScriptEngine engine;

    Fixture() {
        bind();
    }

    void bind() {
        engine.bind(ScriptServices{&registry, &actions, &evaluator, &assets, physics.get()});
    }

    void make_physics() {
        PhysicsWorldDesc desc;
        desc.worker_threads = 0;
        auto created = PhysicsWorld::create(desc);
        REQUIRE(created.ok());
        physics = std::move(created).value();
        bind();
    }
};

// Runs a chunk that must succeed.
void run_ok(ScriptEngine& engine, const char* source) {
    const auto result = engine.run("test", source);
    const std::string detail = result.ok() ? std::string() : result.error().message;
    INFO(detail);
    REQUIRE(result.ok());
    CHECK(engine.stack_depth() == 0);
}

// Runs a chunk that must fail with the given kind, and whose message contains `needle`. Returns the error.
ScriptError run_error(ScriptEngine& engine, const char* source, ScriptErrorKind kind, const char* needle) {
    const auto result = engine.run("test", source);
    REQUIRE_FALSE(result.ok());
    const ScriptError error = result.error();
    INFO(error.message);
    CHECK(error.kind == kind);
    CHECK(error.message.find(needle) != std::string::npos);
    CHECK(engine.stack_depth() == 0);
    return error;
}

Guid guid(const char* text) {
    auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

AssetRecord record(const char* id, const char* path, const char* importer, std::vector<Guid> dependencies = {}) {
    return AssetRecord{guid(id), Path::from_utf8(path).value(), importer, "{}", engine::assets::ContentHash{}, std::move(dependencies)};
}

}  // namespace

TEST_CASE("Scripts run and leave the Lua stack empty on success, syntax errors, and raised errors", "[unit]") {
    ScriptEngine engine;
    CHECK(engine.stack_depth() == 0);
    run_ok(engine, "local total = 0 for i = 1, 10 do total = total + i end assert(total == 55)");
    run_error(engine, "local = 1", ScriptErrorKind::Syntax, "test:1");
    run_error(engine, "error('boom')", ScriptErrorKind::Runtime, "boom");
    run_error(engine, "error({})", ScriptErrorKind::Runtime, "error object is not a string");
}

TEST_CASE("Repeated calls, including failing ones, never leak a value onto the Lua stack", "[unit]") {
    Fixture fixture;
    for (int i = 0; i < 200; ++i) {
        const auto ok = fixture.engine.run("loop", "local e = entity.create() entity.destroy(e)");
        REQUIRE(ok.ok());
        CHECK(fixture.engine.stack_depth() == 0);
        const auto failed = fixture.engine.run("loop", "transform.get(42)");
        REQUIRE_FALSE(failed.ok());
        CHECK(fixture.engine.stack_depth() == 0);
    }
}

TEST_CASE("Type errors from bindings are controlled Lua errors", "[unit]") {
    Fixture fixture;
    fixture.make_physics();
    run_error(fixture.engine, "transform.get(42)", ScriptErrorKind::Runtime, "engine.Entity expected");
    run_error(fixture.engine, "entity.destroy('crate')", ScriptErrorKind::Runtime, "engine.Entity expected");

    run_ok(fixture.engine, "crate = entity.create() entity.add(crate, 'transform')");
    run_error(fixture.engine, "entity.has(crate, 'bogus')", ScriptErrorKind::Runtime, "unknown component");
    run_error(fixture.engine, "transform.set(crate, 3)", ScriptErrorKind::Runtime, "table expected");
    run_error(fixture.engine, "transform.set(crate, {position = {1, 'x', 3}})", ScriptErrorKind::Runtime,
              "transform.position must be three finite numbers");
    run_error(fixture.engine, "transform.set(crate, {scale = {1, 1, 1 / 0}})", ScriptErrorKind::Runtime,
              "transform.scale must be three finite numbers");
    run_error(fixture.engine, "physics.raycast(0, 0, 0, 0, 0, 0, 10)", ScriptErrorKind::Runtime, "direction must not be zero");
    run_error(fixture.engine, "physics.raycast(0, 0, 0, 0, -1, 0, -2)", ScriptErrorKind::Runtime, "distance must be a positive number");
}

TEST_CASE("A destroyed entity raises a controlled error and its slot cannot be reached through a stale handle", "[unit]") {
    Fixture fixture;
    run_ok(fixture.engine, R"LUA(
        first = entity.create()
        entity.add(first, 'transform')
        entity.destroy(first)
        assert(not entity.alive(first))
        second = entity.create()
        assert(entity.alive(second))
        assert(first ~= second)
    )LUA");
    run_error(fixture.engine, "transform.get(first)", ScriptErrorKind::Runtime, "destroyed");
    run_error(fixture.engine, "transform.set(first, {position = {1, 2, 3}})", ScriptErrorKind::Runtime, "destroyed");
    run_error(fixture.engine, "entity.destroy(first)", ScriptErrorKind::Runtime, "destroyed");
    run_error(fixture.engine, "entity.has(first, 'transform')", ScriptErrorKind::Runtime, "destroyed");
    run_ok(fixture.engine, "assert(entity.has(second, 'transform') == false)");
}

TEST_CASE("A host destroys an entity that a script still holds, and the script gets the controlled error", "[unit]") {
    Fixture fixture;
    const EntityHandle crate = fixture.registry.create();
    fixture.registry.add<engine::scene::Transform>(crate);
    fixture.engine.set_global_entity("crate", crate);
    run_ok(fixture.engine, "assert(entity.alive(crate)) assert(transform.get(crate).scale[1] == 1)");

    REQUIRE(fixture.registry.destroy(crate));
    run_error(fixture.engine, "transform.get(crate)", ScriptErrorKind::Runtime, "destroyed");
}

TEST_CASE("Components are added, checked, and removed by name, and the transform reads and writes the registry", "[unit]") {
    Fixture fixture;
    run_ok(fixture.engine, R"LUA(
        crate = entity.create()
        assert(entity.has(crate, 'transform') == false)
        assert(entity.add(crate, 'transform') == true)
        assert(entity.add(crate, 'transform') == false)
        assert(entity.has(crate, 'transform') == true)
    )LUA");
    const EntityHandle crate = [&] {
        std::optional<EntityHandle> found;
        for (const EntityHandle entity : fixture.registry.query<engine::scene::Transform>()) {
            found = entity;
        }
        REQUIRE(found.has_value());
        return *found;
    }();
    CHECK(fixture.registry.has<engine::scene::Transform>(crate));

    fixture.engine.set_global_entity("crate", crate);
    run_ok(fixture.engine, R"LUA(
        transform.set(crate, {position = {1, 2, 3}, rotation = {0, 0, 0, 2}, scale = {2, 2, 2}})
        local t = transform.get(crate)
        assert(t.position[1] == 1 and t.position[2] == 2 and t.position[3] == 3)
        assert(t.scale[3] == 2)
        assert(t.rotation[1] == 0 and t.rotation[4] == 1)
        assert(entity.remove(crate, 'transform') == true)
        assert(entity.remove(crate, 'transform') == false)
    )LUA");
    CHECK(!fixture.registry.has<engine::scene::Transform>(crate));
}

TEST_CASE("A transform write is all or nothing: a malformed field leaves the component unchanged", "[unit]") {
    Fixture fixture;
    const EntityHandle crate = fixture.registry.create();
    fixture.registry.add<engine::scene::Transform>(crate);
    fixture.engine.set_global_entity("crate", crate);

    run_error(fixture.engine, "transform.set(crate, {position = {5, 5, 5}, rotation = {0, 0, 0, 0}})", ScriptErrorKind::Runtime,
              "transform.rotation must not be zero");
    run_error(fixture.engine, "transform.set(crate, {position = {5, 5, 5}, scale = {1, 'x', 1}})", ScriptErrorKind::Runtime,
              "transform.scale must be three finite numbers");

    const engine::scene::Transform* transform = fixture.registry.get<engine::scene::Transform>(crate);
    REQUIRE(transform != nullptr);
    CHECK(transform->position == glm::dvec3(0.0));
    CHECK(transform->rotation == glm::dquat(1.0, 0.0, 0.0, 0.0));
}

TEST_CASE("Scripts read named input actions from the evaluated frame", "[unit]") {
    Fixture fixture;
    const auto added = fixture.actions.add_action("jump", ActionKind::Button, {Binding{KeySource{Key::Space}, 1.0f, 0.0f}});
    REQUIRE(added.ok());

    fixture.input.begin_frame();
    fixture.input.apply(InputEvent{WindowId{}, KeyPressed{Key::Space}});
    fixture.evaluator.update(fixture.actions, fixture.input);

    run_ok(fixture.engine, R"LUA(
        assert(input.down('jump') == true)
        assert(input.pressed('jump') == true)
        assert(input.released('jump') == false)
        assert(input.value('jump') == 1)
    )LUA");
    run_error(fixture.engine, "input.down('missing')", ScriptErrorKind::Runtime, "unknown action 'missing'");
}

TEST_CASE("Scripts query the asset index by path, GUID, importer, and transitive dependencies", "[unit]") {
    Fixture fixture;
    REQUIRE(fixture.assets.add(record(kTextureGuid, "textures/crate.png", "texture")).ok());
    REQUIRE(fixture.assets.add(record(kMeshGuid, "models/crate.gltf", "mesh", {guid(kTextureGuid)})).ok());
    REQUIRE(fixture.assets.add(record(kCrateGuid, "prefabs/crate.prefab", "prefab", {guid(kMeshGuid)})).ok());
    fixture.engine.bind(ScriptServices{&fixture.registry, &fixture.actions, &fixture.evaluator, &fixture.assets, nullptr});

    const std::string script = std::string("mesh = assets.find('models/crate.gltf')\n") +
                               "assert(mesh == '" + kMeshGuid + "')\n" + "assert(assets.exists(mesh))\n" +
                               "assert(assets.importer(mesh) == 'mesh')\n" + "assert(assets.path(mesh) == 'models/crate.gltf')\n" +
                               "local deps = assets.dependencies('" + kCrateGuid + "')\n" + "assert(#deps == 2)\n" +
                               "assert(deps[1] == '" + kMeshGuid + "' or deps[2] == '" + kMeshGuid + "')\n" +
                               "assert(assets.find('missing.png') == nil)\n" + "assert(assets.exists('" + kTextureGuid + "'))\n" +
                               "assert(assets.dependencies('3f2504e0-4f89-41d3-9a0c-0305e82c3399') == nil)\n";
    run_ok(fixture.engine, script.c_str());
    run_error(fixture.engine, "assets.exists('not-a-guid')", ScriptErrorKind::Runtime, "expected a GUID");
}

TEST_CASE("Scripts raycast and overlap the bound physics world, and the hit names the body's entity", "[unit]") {
    Fixture fixture;
    fixture.make_physics();
    const EntityHandle ground = fixture.registry.create();
    BodyDesc ground_body;
    ground_body.entity = ground;
    ground_body.motion = BodyMotion::Static;
    ground_body.shape = ShapeKind::Box;
    ground_body.half_extents = glm::vec3(5.0f, 0.5f, 5.0f);
    ground_body.position = glm::dvec3(0.0, -0.5, 0.0);
    REQUIRE(fixture.physics->add_body(ground_body).ok());
    fixture.engine.set_global_entity("ground", ground);

    run_ok(fixture.engine, R"LUA(
        local hit = physics.raycast(0, 5, 0, 0, -1, 0, 20)
        assert(hit ~= nil)
        assert(hit.entity == ground)
        assert(math.abs(hit.position[2]) < 0.01)
        assert(math.abs(hit.distance - 5) < 0.01)
        assert(hit.normal[2] > 0.99)
        assert(hit.trigger == false)

        assert(physics.raycast(0, 5, 0, 0, -1, 0, 20, {ignore = ground}) == nil)
        assert(physics.raycast(0, 5, 0, 0, -1, 0, 20, {layers = 2}) == nil)
        assert(physics.raycast(0, 5, 0, 0, -1, 0, 2) == nil)

        local found = physics.overlap_sphere(0, 0.1, 0, 0.5)
        assert(#found == 1 and found[1] == ground)
        assert(#physics.overlap_sphere(0, 20, 0, 0.5) == 0)
    )LUA");
    run_error(fixture.engine, "physics.raycast(0, 5, 0, 0, -1, 0, 20, {layers = 'all'})", ScriptErrorKind::Runtime,
              "filter.layers must be an integer");
    run_error(fixture.engine, "physics.raycast(0, 5, 0, 0, -1, 0, 20, {include_triggers = 1})", ScriptErrorKind::Runtime,
              "filter.include_triggers must be a boolean");
}

TEST_CASE("Physics and asset calls raise a controlled error when the service is not bound", "[unit]") {
    ScriptEngine engine;
    run_error(engine, "physics.raycast(0, 5, 0, 0, -1, 0, 20)", ScriptErrorKind::Runtime, "no physics world is bound");
    run_error(engine, "assets.find('a.png')", ScriptErrorKind::Runtime, "no asset registry is bound");
    run_error(engine, "entity.create()", ScriptErrorKind::Runtime, "no entity registry is bound");
    run_error(engine, "input.down('jump')", ScriptErrorKind::Runtime, "no input evaluator is bound");
}

TEST_CASE("Event handlers run in subscription order, a failing handler does not stop the rest, and off removes one", "[unit]") {
    Fixture fixture;
    run_ok(fixture.engine, R"LUA(
        seen = {}
        first = events.on('hit', function(payload) table.insert(seen, payload) end)
        events.on('hit', function(payload) error('handler failed') end)
        events.on('hit', function(payload) table.insert(seen, payload * 2) end)
        events.emit('hit', 21)
        assert(#seen == 2 and seen[1] == 21 and seen[2] == 42)
        assert(events.off(first) == true)
        assert(events.off(first) == false)
        events.emit('hit', 5)
        assert(#seen == 3 and seen[3] == 10)
        events.emit('nobody listens', 1)
    )LUA");

    const std::vector<ScriptError> errors = fixture.engine.take_errors();
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].kind == ScriptErrorKind::Runtime);
    CHECK(errors[0].message.find("handler failed") != std::string::npos);
    CHECK(fixture.engine.take_errors().empty());
    CHECK(fixture.engine.stack_depth() == 0);
}

TEST_CASE("A handler can remove another handler of the same event while it runs", "[unit]") {
    Fixture fixture;
    run_ok(fixture.engine, R"LUA(
        ran = 0
        local second
        events.on('step', function() events.off(second) ran = ran + 1 end)
        second = events.on('step', function() ran = ran + 10 end)
        events.emit('step')
        assert(ran == 1)
        events.emit('step')
        assert(ran == 2)
    )LUA");
    CHECK(fixture.engine.take_errors().empty());
}

TEST_CASE("Event subscription rejects a non-function handler", "[unit]") {
    Fixture fixture;
    run_error(fixture.engine, "events.on('hit', 3)", ScriptErrorKind::Runtime, "function expected");
}

TEST_CASE("The sandbox leaves out the file and OS libraries, and the debug library", "[unit]") {
    ScriptEngine engine;
    run_ok(engine, R"LUA(
        assert(io == nil and os == nil and package == nil and debug == nil)
        assert(dofile == nil and loadfile == nil and require == nil)
        assert(math.floor(2.5) == 2 and string.format('%d', 7) == '7' and utf8.len('abc') == 3)
    )LUA");
}
