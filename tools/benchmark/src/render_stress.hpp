#pragma once

// render_stress (US-076): the GPU render benchmark that engine_bench runs. It builds the scene from the seeded workload
// (engine_benchmark's generate_render_content), draws it along the manifest's camera route with one draw path, and records the
// frame times, the GPU timestamps, the submitted and visible counters, and screenshots. This file is part of the engine_bench
// executable only, so the benchmark library stays free of GPU types.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/report.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace engine::bench::render {

// The draw path of one run. Indexed draws every item with a direct draw. Indirect draws the static and skinned groups with GPU culling
// and indirect count draws. Meshlets sends the static meshes with meshlets through mesh shaders and the rest through the indirect path.
enum class Path : std::uint8_t {
    Indexed,
    Indirect,
    Meshlets,
};

[[nodiscard]] std::string_view to_string(Path path) noexcept;
[[nodiscard]] std::optional<Path> parse_path(std::string_view text) noexcept;

struct RunSettings {
    std::string backend;                    // dx12 or vulkan
    Path path = Path::Indirect;
    bool disable_mesh_shaders = false;      // forces the MeshShaders feature off on the device (the fallback proof)
    std::string shader_dir;                 // the compiled scene shaders (ENGINE_BENCH_SHADER_DIR)
    std::filesystem::path screenshot_dir;   // the start and end screenshots of each repeat are written here
    std::string name;                       // file-name stem of the run, for example render_stress-native_1080-indirect-r1
    double warmup_s = 0.0;
    double duration_s = 0.0;
};

// Measures one repeat of render_stress under one profile and fills `run`: its status and reason, the frame times, the counters, the
// features the run used, and the path. Nothing is reported as passed unless every frame the contract needs was measured.
void run_render_stress(const SceneDef& scene, const std::string& profile_name, const ProfileDef& profile, const RunSettings& settings,
                       RunRecord& run);

}  // namespace engine::bench::render
