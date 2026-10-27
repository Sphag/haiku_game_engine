#include "engine/benchmark/manifest.hpp"

#include "engine/assets/content_hash.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <utility>
#include <set>
#include <sstream>

namespace engine::bench {
namespace {

using Json = nlohmann::json;

constexpr int kManifestVersion = 1;
constexpr std::string_view kFeatureNames[] = {"MeshShaders", "IndirectCount", "RayTracing", "TimestampQueries"};

core::Result<std::string, std::string> string_member(const Json& object, const char* key, const std::string& where) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        return core::failure(where + ": '" + key + "' must be a string");
    }
    return core::success(object.at(key).get<std::string>());
}

core::Result<double, std::string> number_member(const Json& object, const char* key, const std::string& where) {
    if (!object.contains(key) || !object.at(key).is_number()) {
        return core::failure(where + ": '" + key + "' must be a number");
    }
    return core::success(object.at(key).get<double>());
}

// A route's camera (US-076). Every field is required, so a route cannot silently default to the origin.
core::Result<CameraRoute, std::string> parse_camera(const Json& json, const std::string& where) {
    if (!json.is_object()) {
        return core::failure(where + ": camera must be an object");
    }
    if (!json.contains("start_m") || !json.at("start_m").is_array() || json.at("start_m").size() != 2) {
        return core::failure(where + ": 'start_m' must be an array of two numbers");
    }
    CameraRoute camera;
    const Json& start = json.at("start_m");
    if (!start[0].is_number() || !start[1].is_number()) {
        return core::failure(where + ": 'start_m' must hold numbers");
    }
    camera.start_x_m = start[0].get<double>();
    camera.start_z_m = start[1].get<double>();
    const std::pair<const char*, double*> fields[] = {
        {"heading_deg", &camera.heading_deg},
        {"pitch_deg", &camera.pitch_deg},
        {"height_m", &camera.height_m},
        {"speed_m_per_s", &camera.speed_m_per_s},
    };
    for (const auto& [key, target] : fields) {
        const auto value = number_member(json, key, where);
        if (!value.ok()) {
            return core::failure(value.error());
        }
        *target = value.value();
    }
    if (camera.pitch_deg <= -90.0 || camera.pitch_deg >= 90.0) {
        return core::failure(where + ": 'pitch_deg' must be between -90 and 90 exclusive");
    }
    return core::success(camera);
}

core::Result<Timing, std::string> parse_timing(const Json& json, const std::string& where) {
    if (!json.is_object()) {
        return core::failure(where + ": timing must be an object");
    }
    Timing timing;
    for (const char* key : {"warmup_s", "duration_s"}) {
        if (!json.contains(key) || !json.at(key).is_number()) {
            return core::failure(where + ": '" + key + "' must be a number");
        }
    }
    if (!json.contains("repeats") || !json.at("repeats").is_number_integer()) {
        return core::failure(where + ": 'repeats' must be an integer");
    }
    timing.warmup_s = json.at("warmup_s").get<double>();
    timing.duration_s = json.at("duration_s").get<double>();
    timing.repeats = json.at("repeats").get<int>();
    if (!(timing.warmup_s >= 0.0) || !(timing.duration_s > 0.0) || timing.repeats < 1) {
        return core::failure(where + ": warmup must be >= 0, duration > 0, and repeats >= 1");
    }
    return core::success(timing);
}

// Fills the profile's feature list and settings. Feature names must be known, so a typo cannot silently skip a check.
core::Result<ProfileDef, std::string> parse_profile(const Json& json, const std::string& where) {
    if (!json.is_object()) {
        return core::failure(where + ": must be an object");
    }
    ProfileDef profile;
    const auto description = string_member(json, "description", where);
    if (!description.ok()) {
        return core::failure(description.error());
    }
    profile.description = description.value();
    if (!json.contains("frame_contract") || !json.at("frame_contract").is_boolean()) {
        return core::failure(where + ": 'frame_contract' must be a boolean");
    }
    profile.frame_contract = json.at("frame_contract").get<bool>();
    if (!json.contains("requires_features") || !json.at("requires_features").is_array()) {
        return core::failure(where + ": 'requires_features' must be an array");
    }
    for (const auto& name : json.at("requires_features")) {
        bool known = false;
        for (const auto candidate : kFeatureNames) {
            known = known || (name.is_string() && name.get<std::string>() == candidate);
        }
        if (!known) {
            return core::failure(where + ": unknown feature in requires_features: " + name.dump());
        }
        profile.requires_features.push_back(name.get<std::string>());
    }
    if (!json.contains("settings") || !json.at("settings").is_object()) {
        return core::failure(where + ": 'settings' must be an object");
    }
    profile.settings_json = json.at("settings").dump();
    if (json.at("settings").contains("output")) {
        const auto output = string_member(json.at("settings"), "output", where + ".settings");
        if (!output.ok()) {
            return core::failure(output.error());
        }
        // WIDTHxHEIGHT, both positive; anything else is refused so a profile cannot name an output the driver would guess at.
        unsigned long width = 0;
        unsigned long height = 0;
        char separator = 0;
        std::istringstream text(output.value());
        const bool leading_digit = !output.value().empty() && std::isdigit(static_cast<unsigned char>(output.value()[0])) != 0;
        if (!leading_digit || !(text >> width >> separator >> height) || separator != 'x' || width == 0 || height == 0 ||
            !(text >> std::ws).eof()) {
            return core::failure(where + ".settings: 'output' must be WIDTHxHEIGHT");
        }
        profile.output_width = static_cast<std::uint32_t>(width);
        profile.output_height = static_cast<std::uint32_t>(height);
    }
    return core::success(profile);
}

core::Result<SceneDef, std::string> parse_scene(const Json& json, const std::string& where) {
    if (!json.is_object()) {
        return core::failure(where + ": must be an object");
    }
    SceneDef scene;
    const auto implementation = string_member(json, "implementation", where);
    if (!implementation.ok()) {
        return core::failure(implementation.error());
    }
    if (implementation.value() == "builtin") {
        scene.builtin = true;
    } else if (implementation.value() != "none") {
        return core::failure(where + ": implementation must be \"builtin\" or \"none\"");
    }
    if (!json.contains("seed") || !json.at("seed").is_number_unsigned()) {
        return core::failure(where + ": 'seed' must be a non-negative integer");
    }
    scene.seed = json.at("seed").get<std::uint64_t>();
    if (!json.contains("workload") || !json.at("workload").is_object()) {
        return core::failure(where + ": 'workload' must be an object");
    }
    for (const auto& [key, value] : json.at("workload").items()) {
        if (value.is_number_unsigned()) {
            scene.counts[key] = value.get<std::uint64_t>();
        }
    }
    scene.definition_json = json.dump();
    if (!json.contains("route")) {
        return core::failure(where + ": 'route' is required (null when the scene has none)");
    }
    const Json& route = json.at("route");
    if (route.is_null()) {
        scene.route_status = "none";
    } else if (route.is_object()) {
        const auto name = string_member(route, "name", where + ".route");
        if (!name.ok()) {
            return core::failure(name.error());
        }
        scene.route = name.value();
        const auto definition = string_member(route, "definition", where + ".route");
        if (!definition.ok()) {
            return core::failure(definition.error());
        }
        scene.route_status = definition.value();
        // A route without a camera (the pending ones) is allowed; the scene that needs the camera checks for it.
        if (route.contains("camera")) {
            const auto camera = parse_camera(route.at("camera"), where + ".route.camera");
            if (!camera.ok()) {
                return core::failure(camera.error());
            }
            scene.camera = camera.value();
        }
    } else {
        return core::failure(where + ": 'route' must be null or an object");
    }
    if (scene.builtin && (scene.counts.empty() || scene.counts.count("items") == 0 || scene.counts.count("rounds_per_frame") == 0)) {
        return core::failure(where + ": a builtin scene needs 'items' and 'rounds_per_frame' in its workload");
    }
    return core::success(scene);
}

}  // namespace

core::Result<Manifest, std::string> parse_manifest(std::string_view text) {
    Json root;
    try {
        root = Json::parse(text.begin(), text.end());
    } catch (const Json::exception& error) {
        return core::failure(std::string("manifest is not valid JSON: ") + error.what());
    }
    if (!root.is_object() || !root.contains("manifest_version") || !root.at("manifest_version").is_number_integer() ||
        root.at("manifest_version").get<int>() != kManifestVersion) {
        return core::failure("manifest_version must be " + std::to_string(kManifestVersion));
    }

    Manifest manifest;
    manifest.sha256 = assets::ContentHash::of(text).to_hex();
    if (!root.contains("world") || !root.at("world").is_object()) {
        return core::failure(std::string("'world' must be an object"));
    }
    manifest.world_json = root.at("world").dump();

    if (!root.contains("profiles") || !root.at("profiles").is_object()) {
        return core::failure(std::string("'profiles' must be an object"));
    }
    for (const auto& [name, json] : root.at("profiles").items()) {
        auto profile = parse_profile(json, "profile " + name);
        if (!profile.ok()) {
            return core::failure(profile.error());
        }
        manifest.profiles[name] = profile.value();
    }

    if (!root.contains("scenes") || !root.at("scenes").is_object()) {
        return core::failure(std::string("'scenes' must be an object"));
    }
    for (const auto& [name, json] : root.at("scenes").items()) {
        auto scene = parse_scene(json, "scene " + name);
        if (!scene.ok()) {
            return core::failure(scene.error());
        }
        manifest.scenes[name] = scene.value();
    }

    if (!root.contains("suites") || !root.at("suites").is_object()) {
        return core::failure(std::string("'suites' must be an object"));
    }
    for (const auto& [name, json] : root.at("suites").items()) {
        const std::string where = "suite " + name;
        if (!json.is_object()) {
            return core::failure(where + ": must be an object");
        }
        SuiteDef suite;
        const auto description = string_member(json, "description", where);
        if (!description.ok()) {
            return core::failure(description.error());
        }
        suite.description = description.value();
        auto timing = parse_timing(json.contains("timing") ? json.at("timing") : Json(), where);
        if (!timing.ok()) {
            return core::failure(timing.error());
        }
        suite.timing = timing.value();
        if (name == "acceptance" && (suite.timing.warmup_s != 30.0 || suite.timing.duration_s != 180.0 || suite.timing.repeats != 3)) {
            return core::failure(where + ": the acceptance timing is fixed by the performance contract (30 s warmup, 180 s, 3 repeats)");
        }
        if (!json.contains("runs") || !json.at("runs").is_array() || json.at("runs").empty()) {
            return core::failure(where + ": 'runs' must be a non-empty array");
        }
        for (const auto& run : json.at("runs")) {
            if (!run.is_object() || !run.contains("scene") || !run.contains("profile") || !run.at("scene").is_string() ||
                !run.at("profile").is_string()) {
                return core::failure(where + ": each run needs string 'scene' and 'profile'");
            }
            RunDef def{run.at("scene").get<std::string>(), run.at("profile").get<std::string>()};
            if (manifest.scenes.count(def.scene) == 0) {
                return core::failure(where + ": run names an unknown scene '" + def.scene + "'");
            }
            if (manifest.profiles.count(def.profile) == 0) {
                return core::failure(where + ": run names an unknown profile '" + def.profile + "'");
            }
            suite.runs.push_back(def);
        }
        manifest.suites[name] = suite;
    }
    return core::success(manifest);
}

}  // namespace engine::bench
