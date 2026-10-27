// Saved input profiles (US-011): round trip, format and version checks, name and value rejection, and file IO. CPU-only,
// runs on every operating system.

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/platform/action_map.hpp"
#include "engine/platform/action_profile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

using engine::core::Path;
using engine::platform::ActionErrorCode;
using engine::platform::ActionId;
using engine::platform::ActionKind;
using engine::platform::ActionMap;
using engine::platform::Binding;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisSource;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonSource;
using engine::platform::Key;
using engine::platform::KeySource;
using engine::platform::MouseButton;
using engine::platform::MouseButtonSource;
using engine::platform::parse_profile;
using engine::platform::serialize_profile;

namespace {

Binding key(Key value, float scale = 1.0f) {
    return Binding{KeySource{value}, scale, 0.0f};
}

// The same three actions in every map, so a profile saved from one applies to another.
void define_actions(ActionMap& map) {
    REQUIRE(map.add_action("jump", ActionKind::Button, {key(Key::Space)}).ok());
    REQUIRE(map.add_action("move_x", ActionKind::Axis, {key(Key::D), key(Key::A, -1.0f)}).ok());
    REQUIRE(map.add_action("fire", ActionKind::Button, {Binding{MouseButtonSource{MouseButton::Left}, 1.0f, 0.0f}})
                .ok());
}

// Builds a map with user overrides on every action, including a stick with a dead zone and a pad slot.
ActionMap remapped_map() {
    ActionMap map;
    define_actions(map);
    const ActionId jump = *map.find("jump");
    const ActionId move_x = *map.find("move_x");
    const ActionId fire = *map.find("fire");
    REQUIRE(map.rebind(jump, {key(Key::Enter)}).ok());
    REQUIRE(map.rebind(move_x, {key(Key::Right), key(Key::Left, -1.0f),
                                Binding{GamepadAxisSource{GamepadAxis::LeftX, 0u}, 1.0f, 0.2f}})
                .ok());
    REQUIRE(map.rebind(fire, {Binding{MouseButtonSource{MouseButton::Left}, 1.0f, 0.0f},
                              Binding{GamepadButtonSource{GamepadButton::A, 1u}, 1.0f, 0.0f}})
                .ok());
    return map;
}

// A scratch directory under the system temp location, removed when the test ends.
struct ScratchDir {
    std::filesystem::path root;

    ScratchDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("engine_input_profile_" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

Path path_from(const std::string& text) {
    const auto parsed = Path::from_utf8(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

}  // namespace

TEST_CASE("Profile: overrides survive a round trip with the same text", "[unit]") {
    const ActionMap source = remapped_map();
    const std::string text = serialize_profile(source.overrides());

    CHECK(contains(text, "\"format\": \"engine.input_profile\""));
    CHECK(contains(text, "\"version\": 1"));
    // Values are written as the shortest decimal that reads back exactly.
    CHECK(contains(text, "\"dead_zone\": 0.2"));

    const auto parsed = parse_profile(text);
    REQUIRE(parsed.ok());

    ActionMap restored;
    define_actions(restored);
    REQUIRE(restored.apply_overrides(parsed.value()).ok());
    CHECK(serialize_profile(restored.overrides()) == text);
    CHECK(restored.has_override(*restored.find("jump")));
    CHECK(restored.has_override(*restored.find("fire")));
}

TEST_CASE("Profile: an action with no entry keeps its defaults", "[unit]") {
    ActionMap map;
    define_actions(map);
    REQUIRE(map.rebind(*map.find("jump"), {key(Key::Enter)}).ok());

    const std::string text = serialize_profile(map.overrides());
    CHECK_FALSE(contains(text, "\"move_x\""));

    ActionMap restored;
    define_actions(restored);
    REQUIRE(restored.apply_overrides(parse_profile(text).value()).ok());
    CHECK_FALSE(restored.has_override(*restored.find("move_x")));
    CHECK(restored.bindings(*restored.find("move_x")).size() == 2);
}

TEST_CASE("Profile: an empty profile is valid and applies as a full reset", "[unit]") {
    ActionMap map = remapped_map();
    const auto parsed = parse_profile(serialize_profile({}));
    REQUIRE(parsed.ok());
    CHECK(parsed.value().empty());

    REQUIRE(map.apply_overrides(parsed.value()).ok());
    CHECK(map.overrides().empty());
}

TEST_CASE("Profile: wrong format, unsupported version, and bad syntax are rejected", "[unit]") {
    const auto wrong_format = parse_profile(R"({"format": "other", "version": 1, "actions": {}})");
    REQUIRE_FALSE(wrong_format.ok());
    CHECK(wrong_format.error().code == ActionErrorCode::MalformedProfile);

    const auto newer = parse_profile(R"({"format": "engine.input_profile", "version": 2, "actions": {}})");
    REQUIRE_FALSE(newer.ok());
    CHECK(newer.error().code == ActionErrorCode::UnsupportedVersion);
    CHECK(contains(newer.error().message, "version 2"));

    const auto no_version = parse_profile(R"({"format": "engine.input_profile", "actions": {}})");
    REQUIRE_FALSE(no_version.ok());
    CHECK(no_version.error().code == ActionErrorCode::MalformedProfile);

    const auto not_json = parse_profile("{ format: ");
    REQUIRE_FALSE(not_json.ok());
    CHECK(not_json.error().code == ActionErrorCode::MalformedProfile);

    const auto not_object = parse_profile("[1, 2]");
    REQUIRE_FALSE(not_object.ok());
    CHECK(not_object.error().code == ActionErrorCode::MalformedProfile);

    const auto no_actions = parse_profile(R"({"format": "engine.input_profile", "version": 1})");
    REQUIRE_FALSE(no_actions.ok());
    CHECK(no_actions.error().code == ActionErrorCode::MalformedProfile);
}

TEST_CASE("Profile: unknown sources and names are rejected with the action named", "[unit]") {
    const auto bad_source = parse_profile(
        R"({"format": "engine.input_profile", "version": 1, "actions": {"jump": [{"source": "joystick"}]}})");
    REQUIRE_FALSE(bad_source.ok());
    CHECK(bad_source.error().code == ActionErrorCode::MalformedProfile);
    CHECK(contains(bad_source.error().message, "action 'jump'"));
    CHECK(contains(bad_source.error().message, "joystick"));

    const auto bad_key = parse_profile(
        R"({"format": "engine.input_profile", "version": 1, "actions": {"jump": [{"source": "key", "key": "Hyper"}]}})");
    REQUIRE_FALSE(bad_key.ok());
    CHECK(contains(bad_key.error().message, "Hyper"));

    const auto missing_name = parse_profile(
        R"({"format": "engine.input_profile", "version": 1, "actions": {"jump": [{"source": "key"}]}})");
    REQUIRE_FALSE(missing_name.ok());
    CHECK(contains(missing_name.error().message, "'key'"));

    const auto negative_slot = parse_profile(
        R"({"format": "engine.input_profile", "version": 1, "actions": {"a": [{"source": "gamepad_button", "button": "A", "slot": -1}]}})");
    REQUIRE_FALSE(negative_slot.ok());
    CHECK(contains(negative_slot.error().message, "slot"));
}

TEST_CASE("Profile: applying a profile with an unknown action changes nothing", "[unit]") {
    ActionMap map = remapped_map();
    const std::string before = serialize_profile(map.overrides());

    const auto parsed = parse_profile(
        R"({"format": "engine.input_profile", "version": 1, "actions": {"reload": [{"source": "key", "key": "R"}]}})");
    REQUIRE(parsed.ok());

    const auto applied = map.apply_overrides(parsed.value());
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().code == ActionErrorCode::UnknownAction);
    CHECK(contains(applied.error().message, "reload"));
    CHECK(serialize_profile(map.overrides()) == before);
}

TEST_CASE("Profile: applying a profile with an invalid value changes nothing", "[unit]") {
    ActionMap map = remapped_map();
    const std::string before = serialize_profile(map.overrides());

    // The second binding of "jump" names a pad slot that does not exist. The parse accepts the number; the apply does
    // not, and the whole profile is refused.
    const auto parsed = parse_profile(R"({"format": "engine.input_profile", "version": 1, "actions": {
        "jump": [{"source": "key", "key": "Q"}],
        "fire": [{"source": "gamepad_button", "button": "A", "slot": 9}]}})");
    REQUIRE(parsed.ok());

    const auto applied = map.apply_overrides(parsed.value());
    REQUIRE_FALSE(applied.ok());
    CHECK(applied.error().code == ActionErrorCode::InvalidBinding);
    CHECK(contains(applied.error().message, "action 'fire'"));
    CHECK(serialize_profile(map.overrides()) == before);
}

TEST_CASE("Profile: a file saves atomically and loads back", "[unit]") {
    ScratchDir scratch;
    const Path path = path_from(scratch.root.string() + "/input_profile.json");
    const ActionMap source = remapped_map();

    const auto saved = engine::platform::save_profile(path, source.overrides());
    REQUIRE(saved.ok());
    CHECK_FALSE(engine::core::path_exists(path_from(path.str() + ".tmp")));

    const auto loaded = engine::platform::load_profile(path);
    REQUIRE(loaded.ok());
    CHECK(serialize_profile(loaded.value()) == serialize_profile(source.overrides()));
}

TEST_CASE("Profile: loading a missing file reports an IO error", "[unit]") {
    ScratchDir scratch;
    const Path missing = path_from(scratch.root.string() + "/absent.json");

    const auto loaded = engine::platform::load_profile(missing);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().code == ActionErrorCode::Io);
}
