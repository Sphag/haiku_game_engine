#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bench {

struct Timing {
    double warmup_s = 0.0;
    double duration_s = 0.0;
    int repeats = 0;
};

struct ProfileDef {
    std::string description;
    bool frame_contract = false;             // the FPS contract applies to runs with this profile
    std::vector<std::string> requires_features;  // RHI feature names; a missing one makes the run unsupported
    std::string settings_json;               // canonical dump of the profile's settings, echoed into the report
    std::uint32_t output_width = 0;          // settings.output as WIDTHxHEIGHT; zero when the profile names no output
    std::uint32_t output_height = 0;
};

// A camera route (US-076): the camera starts at (start_x_m, start_z_m) above the terrain, looks along the heading, and moves at a constant
// speed. The frame index sets the position (frame / 60 seconds), so every run sees the same camera at the same frame.
struct CameraRoute {
    double start_x_m = 0.0;
    double start_z_m = 0.0;
    double heading_deg = 0.0;        // 0 looks down -Z, 90 looks down +X
    double pitch_deg = 0.0;          // negative looks down
    double height_m = 0.0;           // above the terrain at the camera
    double speed_m_per_s = 0.0;
};

struct SceneDef {
    bool builtin = false;                    // implemented by engine_bench itself; otherwise the workload is not built yet
    std::uint64_t seed = 0;
    std::map<std::string, std::uint64_t> counts;  // integer workload parameters (the expected counts)
    std::string definition_json;             // canonical dump of the whole scene (workload and route), echoed into the report
    std::string route;                       // route name, empty when the scene has none
    std::string route_status;                // "none", or the route's definition text
    std::optional<CameraRoute> camera;       // set when the route has a camera definition
};

struct RunDef {
    std::string scene;
    std::string profile;
};

struct SuiteDef {
    std::string description;
    Timing timing;
    std::vector<RunDef> runs;
};

struct Manifest {
    std::string sha256;                      // hash of the manifest text, so a report names the exact contract it used
    std::string world_json;
    std::map<std::string, ProfileDef> profiles;
    std::map<std::string, SceneDef> scenes;
    std::map<std::string, SuiteDef> suites;
};

// Parses and checks a manifest. The acceptance suite must carry the contract timing (30 s warmup, 180 s, 3 repeats).
// The error names the first problem found.
[[nodiscard]] core::Result<Manifest, std::string> parse_manifest(std::string_view text);

}  // namespace engine::bench
