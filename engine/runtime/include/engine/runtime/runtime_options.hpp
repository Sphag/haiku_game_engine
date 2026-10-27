#pragma once

// Command-line options of the standalone runtime (US-060). The runtime takes the packs to load, the start scene (a root of one
// of those packs), the render backend, and the profile. Every value is checked here, before any file is opened, so a bad
// argument never starts a partial load.
//
//   game_runtime --pack <file.pack> [--pack <file.pack>...] --start-scene <guid> [--backend dx12|vulkan] [--profile native|full]
//
// The backend defaults to dx12 on Windows and vulkan elsewhere. The profile defaults to native (PRD FR-09).

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace engine::runtime {

inline constexpr std::string_view kBackendDx12 = "dx12";
inline constexpr std::string_view kBackendVulkan = "vulkan";
inline constexpr std::string_view kProfileNative = "native";
inline constexpr std::string_view kProfileFull = "full";

struct RuntimeOptions {
    std::vector<std::string> packs;  // in the order given
    core::Guid start_scene;          // nil until --start-scene is given
    std::string backend;
    std::string profile = std::string(kProfileNative);
    bool help = false;
};

// The usage text, for --help and for usage errors.
[[nodiscard]] std::string runtime_usage_text();

// Parses the arguments after the program name. Returns the message for a usage error. Help returns options with help set,
// and no other check is run.
[[nodiscard]] core::Result<RuntimeOptions, std::string> parse_runtime_options(const std::vector<std::string>& args);

}  // namespace engine::runtime
