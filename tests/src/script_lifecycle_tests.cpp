// The script lifecycle (US-056) on the CPU: callback order, per-instance error isolation, reload with and without preserved exports,
// and the teardown guarantees for Play/Stop, entity destruction (streaming), and event subscriptions. Each chunk records what ran in
// the global `log`, and the checks read it back through a Lua assertion. The tests run on every operating system with no renderer.

#include "engine/ecs/registry.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::ecs::EntityHandle;
using engine::ecs::Registry;
using engine::script::InstanceState;
using engine::script::ReloadPolicy;
using engine::script::ScriptEngine;
using engine::script::ScriptError;
using engine::script::ScriptErrorKind;
using engine::script::ScriptId;
using engine::script::ScriptRunner;
using engine::script::ScriptServices;

namespace {

// Prepended to every chunk: `note` appends to the global log. Writes through _G reach the shared globals from any instance.
constexpr const char* kNote = "local function note(s) _G.log = (_G.log or '') .. s .. ',' end\n";

struct Fixture {
    Registry registry;
    ScriptEngine engine;
    ScriptRunner runner{engine, registry};

    Fixture() {
        ScriptServices services;
        services.registry = &registry;
        engine.bind(services);
        run_ok("_G.log = ''");
    }

    void run_ok(const std::string& source) {
        const auto result = engine.run("test", source);
        const std::string detail = result.ok() ? std::string() : result.error().message;
        INFO(detail);
        REQUIRE(result.ok());
        CHECK(engine.stack_depth() == 0);
    }

    // Checks the log with a Lua assertion, so a mismatch reports the actual text.
    void expect_log(const std::string& expected) {
        const std::string check = "assert((_G.log or '') == '" + expected + "', 'log is: ' .. tostring(_G.log))";
        run_ok(check);
    }

    ScriptId attach(const std::string& body, ReloadPolicy policy = ReloadPolicy::PreserveExports) {
        const auto result = runner.attach(registry.create(), "test", std::string(kNote) + body, policy);
        const std::string detail = result.ok() ? std::string() : result.error().message;
        INFO(detail);
        REQUIRE(result.ok());
        return result.value();
    }
};

}  // namespace

TEST_CASE("Callbacks run in the documented order: create and start at the next frame, then fixed_update and update, then destroy",
          "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        function create(e) note('create') end
        function start(e) note('start') end
        function fixed_update(e, dt) note('fixed') end
        function update(e, dt) note('update') end
        function destroy(e) note('destroy') end
    )");
    f.expect_log("");  // attach runs no callback
    f.runner.begin_frame();
    f.expect_log("create,start,");
    CHECK(f.runner.state(id) == InstanceState::Running);
    f.runner.fixed_update(1.0 / 60.0);
    f.runner.update(1.0 / 60.0);
    f.expect_log("create,start,fixed,update,");
    CHECK(f.runner.detach(id));
    f.expect_log("create,start,fixed,update,destroy,");
    CHECK(f.runner.instance_count() == 0);
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("A callback the chunk does not define is not taken from the shared globals", "[unit]") {
    Fixture f;
    f.run_ok("function update(e, dt) note('shared') end");  // a global, not part of any instance
    f.attach("function start(e) note('start') end");
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.expect_log("start,");
}

TEST_CASE("An instance detached before its first frame runs neither create nor destroy", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        function create(e) note('create') end
        function destroy(e) note('destroy') end
    )");
    CHECK(f.runner.detach(id));
    f.runner.begin_frame();
    f.expect_log("");
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("A raised error stops only its instance, is reported, and the other instances keep running", "[unit]") {
    Fixture f;
    const ScriptId bad = f.attach("function update(e, dt) note('bad') error('boom') end");
    const ScriptId good = f.attach("function update(e, dt) note('good') end");
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.expect_log("bad,good,");

    const std::vector<ScriptError> errors = f.runner.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].kind == ScriptErrorKind::Runtime);
    CHECK(errors[0].message.find("update: ") == 0);
    CHECK(errors[0].message.find("boom") != std::string::npos);
    CHECK(f.runner.state(bad) == InstanceState::Faulted);
    CHECK(f.runner.state(good) == InstanceState::Running);

    f.runner.update(0.1);
    f.expect_log("bad,good,good,");
    CHECK(f.runner.take_errors().empty());
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("A raised error in create faults the instance: start and destroy do not run", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        function create(e) error('no create') end
        function start(e) note('start') end
        function destroy(e) note('destroy') end
    )");
    f.runner.begin_frame();
    CHECK(f.runner.state(id) == InstanceState::Faulted);
    const std::vector<ScriptError> errors = f.runner.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.find("create: ") == 0);
    CHECK(f.runner.detach(id));
    f.expect_log("");
}

TEST_CASE("A syntax or top-level error fails attach and leaves no instance or subscription behind", "[unit]") {
    Fixture f;
    const auto syntax = f.runner.attach(f.registry.create(), "broken", "local = 1");
    REQUIRE_FALSE(syntax.ok());
    CHECK(syntax.error().kind == ScriptErrorKind::Syntax);

    const auto top = f.runner.attach(f.registry.create(), "top", "events.on('ping', function() end) error('top level')");
    REQUIRE_FALSE(top.ok());
    CHECK(top.error().kind == ScriptErrorKind::Runtime);
    CHECK(top.error().message.find("top level") != std::string::npos);

    CHECK(f.runner.instance_count() == 0);
    CHECK(f.engine.events().size() == 0);
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("Attaching to a destroyed entity fails with a controlled error", "[unit]") {
    Fixture f;
    const EntityHandle entity = f.registry.create();
    REQUIRE(f.registry.destroy(entity));
    const auto result = f.runner.attach(entity, "dead", "");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().kind == ScriptErrorKind::Runtime);
    CHECK(f.runner.instance_count() == 0);
}

TEST_CASE("Reload with PreserveExports passes the exported state to the new code, which runs create and start again", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        local counter = 0
        function export_state() return {count = counter} end
        function import_state(s) counter = s.count end
        function create(e) note('create') end
        function start(e) note('start') end
        function update(e, dt) counter = counter + 1 note('u' .. counter) end
        function destroy(e) note('destroy') end
    )");
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.runner.update(0.1);
    f.runner.update(0.1);
    f.expect_log("create,start,u1,u2,u3,");

    const auto reloaded = f.runner.reload(id, "v2", std::string(kNote) + R"(
        local counter = 0
        function import_state(s) counter = s.count end
        function create(e) note('create2') end
        function start(e) note('start2') end
        function update(e, dt) counter = counter + 10 note('v2:' .. counter) end
        function destroy(e) note('destroy2') end
    )");
    REQUIRE(reloaded.ok());
    // The old destroy runs before the switch; the new create and start wait for the next frame.
    f.expect_log("create,start,u1,u2,u3,destroy,");
    CHECK(f.runner.state(id) == InstanceState::Pending);
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.expect_log("create,start,u1,u2,u3,destroy,create2,start2,v2:13,");
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("Reload with ResetState starts the new code from its own initial values", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        local counter = 0
        function export_state() return {count = counter} end
        function import_state(s) counter = s.count end
        function update(e, dt) counter = counter + 1 note('u' .. counter) end
    )", ReloadPolicy::ResetState);
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.runner.update(0.1);
    const auto reloaded = f.runner.reload(id, "v2", std::string(kNote) + R"(
        local counter = 0
        function import_state(s) counter = s.count end
        function update(e, dt) counter = counter + 10 note('v2:' .. counter) end
    )");
    REQUIRE(reloaded.ok());
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.expect_log("u1,u2,v2:10,");
}

TEST_CASE("A reload whose code does not compile leaves the running instance and its state unchanged", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach(R"(
        local counter = 0
        function export_state() return {count = counter} end
        function update(e, dt) counter = counter + 1 note('u' .. counter) end
    )");
    f.runner.begin_frame();
    f.runner.update(0.1);
    const auto reloaded = f.runner.reload(id, "broken", "local = 1");
    REQUIRE_FALSE(reloaded.ok());
    CHECK(reloaded.error().kind == ScriptErrorKind::Syntax);
    CHECK(f.runner.state(id) == InstanceState::Running);
    f.runner.update(0.1);
    f.expect_log("u1,u2,");
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("A faulted instance recovers when a repaired chunk is reloaded", "[unit]") {
    Fixture f;
    const ScriptId id = f.attach("function update(e, dt) error('bad') end");
    f.runner.begin_frame();
    f.runner.update(0.1);
    CHECK(f.runner.state(id) == InstanceState::Faulted);
    CHECK(f.runner.take_errors().size() == 1);

    const auto reloaded = f.runner.reload(id, "fixed", std::string(kNote) + "function update(e, dt) note('ok') end");
    REQUIRE(reloaded.ok());
    CHECK(f.runner.state(id) == InstanceState::Pending);
    f.runner.begin_frame();
    f.runner.update(0.1);
    f.expect_log("ok,");
}

TEST_CASE("Repeated Play/Stop cycles leave no instances, subscriptions, or Lua references", "[unit]") {
    Fixture f;
    f.run_ok("_G.collected = 0");
    // Each instance holds a value with a finalizer, in its callbacks and in its event handler. Once the runner and the bus release
    // the instance, a full collection must finalize every value.
    const std::string chunk = R"(
        local token = setmetatable({}, {__gc = function() _G.collected = _G.collected + 1 end})
        events.on('tick', function() local _ = token end)
        function update(e, dt) local _ = token end
    )";
    for (int cycle = 0; cycle < 20; ++cycle) {
        REQUIRE(f.runner.attach(f.registry.create(), "a", chunk).ok());
        REQUIRE(f.runner.attach(f.registry.create(), "b", chunk).ok());
        f.runner.begin_frame();
        f.runner.update(0.1);
        f.run_ok("events.emit('tick')");
        CHECK(f.engine.events().size() == 2);
        f.runner.stop_all();
        CHECK(f.runner.instance_count() == 0);
        CHECK(f.engine.events().size() == 0);
    }
    f.run_ok("collectgarbage('collect')");
    f.run_ok("assert(_G.collected == 40, 'collected ' .. tostring(_G.collected))");
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("An entity destroyed by streaming is torn down at the next frame without destroy, and its subscriptions go with it",
          "[unit]") {
    Fixture f;
    const EntityHandle entity = f.registry.create();
    const auto attached = f.runner.attach(entity, "streamed", std::string(kNote) + R"(
        events.on('tick', function() note('tick') end)
        function update(e, dt) note('update') end
        function destroy(e) note('destroy') end
    )");
    REQUIRE(attached.ok());
    f.runner.begin_frame();
    f.runner.update(0.1);
    CHECK(f.engine.events().size() == 1);

    REQUIRE(f.registry.destroy(entity));
    f.runner.begin_frame();
    CHECK(f.runner.instance_count() == 0);
    CHECK(f.engine.events().size() == 0);
    f.expect_log("update,");
    CHECK(f.engine.stack_depth() == 0);
}

TEST_CASE("Detaching one instance releases only its subscriptions; the other instance's handler keeps running", "[unit]") {
    Fixture f;
    const ScriptId a = f.attach("events.on('ping', function() note('A') end)");
    f.attach("events.on('ping', function() note('B') end)");
    f.runner.begin_frame();
    CHECK(f.runner.detach(a));
    CHECK(f.engine.events().size() == 1);
    f.run_ok("events.emit('ping')");
    f.expect_log("B,");
}

TEST_CASE("stop_all destroys only the running instances, in attach order, and leaves nothing attached", "[unit]") {
    Fixture f;
    f.attach("function create(e) note('create1') end function destroy(e) note('destroy1') end");
    f.runner.begin_frame();
    f.attach("function destroy(e) note('destroy2') end");  // still pending: attached after the frame
    f.runner.stop_all();
    f.expect_log("create1,destroy1,");
    CHECK(f.runner.instance_count() == 0);
    CHECK(f.engine.stack_depth() == 0);
}
