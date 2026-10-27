#pragma once

#include "engine/benchmark/manifest.hpp"
#include "engine/core/result.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bench {

// Seed-derived benchmark workloads (US-075). The generator reads each scene's integer counts from the benchmark table
// (benchmarks/manifest.json) and produces that content from the scene seed: unique meshes, instance descriptors, animated
// characters with clips, lights and suns, particle emitters, physics bodies and constraints, audio emitters, and the shared
// terrain window. Every value is an integer (millimetres, millidegrees, permille), so the bytes and their SHA-256 digests are
// the same on every compiler and host, and no file is downloaded or read from outside the repository.
//
// A workload is verified against an expected file (benchmarks/workload_expected.json) that pins its counts and the digest of
// each component. Counts that the table names are checked against what was generated, so a mismatch is an error, not a
// quietly different workload.

inline constexpr int kWorkloadVersion = 2;

// The shared world constants (the manifest's world block). The generator uses these, and a test checks them against the
// manifest so the two cannot drift apart.
struct GeneratorWorld {
    std::int64_t size_m = 0;
    std::int64_t cell_m = 0;
    std::int64_t grid_cells = 0;
    std::int64_t window_cells = 0;           // the detailed residency window, centred on the world
    std::int64_t tile_samples = 0;           // height samples per tile edge (cell_m + 1 at 1 m spacing)
    std::uint64_t world_seed = 0;            // seeds the terrain and the placement of every scene's content
};

[[nodiscard]] GeneratorWorld generator_world();

// Checks the manifest's world block (its JSON text) against the generator's constants. A mismatch is an error: the generator
// builds the world from its own constants, so the table's world block must describe the same world.
[[nodiscard]] core::Result<core::Unit, std::string> check_generator_world(std::string_view world_json);

// One generated workload. `counts` holds the table counts that were generated, plus derived checkpoints such as the number
// of instances within the draw distance of the world centre. `sha256` holds one digest per component, always present for
// every component (an empty component hashes the empty input).
struct Workload {
    std::string scene;
    std::uint64_t seed = 0;
    std::uint64_t world_seed = 0;
    std::map<std::string, std::uint64_t> counts;
    std::map<std::string, std::string> sha256;
};

// Generates the workload of a scene from its table entry. Fails for a builtin scene (it is not a generated workload), for a
// workload key the generator does not know and the table does not mark as a runtime parameter, and for a table count that
// the generated content does not reproduce.
[[nodiscard]] core::Result<Workload, std::string> generate_workload(std::string_view scene_name, const SceneDef& scene);

// The physics content of a scene (US-077): the bodies, static colliders, constraints, and characters that the `physics` digest of
// generate_workload covers, as integers in millimetres and permille. The physics runner builds its world from these values, so the
// digest and the simulated scene are the same data. Mass is generated and digested, but the adapter derives mass from the shape, so the
// runner does not apply it.
struct PhysicsBodySpec {
    std::array<std::int64_t, 3> position_mm{};
    std::array<std::uint32_t, 3> half_extent_mm{};
    std::uint32_t mass_permille = 0;
};

struct PhysicsStaticSpec {
    std::array<std::int64_t, 3> position_mm{};
    std::array<std::uint32_t, 3> half_extent_mm{};
};

struct PhysicsConstraintSpec {
    std::uint32_t body_a = 0;  // index into PhysicsSpec::bodies
    std::uint32_t body_b = 0;  // a different index
};

struct PhysicsCharacterSpec {
    std::array<std::int64_t, 3> position_mm{};  // the bottom of the capsule
    std::uint32_t radius_mm = 0;
};

struct PhysicsSpec {
    std::int64_t centre_mm = 0;  // x and z of the world centre, where the bodies are placed
    std::vector<PhysicsBodySpec> bodies;
    std::vector<PhysicsStaticSpec> statics;
    std::vector<PhysicsConstraintSpec> constraints;
    std::vector<PhysicsCharacterSpec> characters;
};

// Generates the physics content of a scene from its table entry, under the same rules as generate_workload.
[[nodiscard]] core::Result<PhysicsSpec, std::string> generate_physics_spec(std::string_view scene_name, const SceneDef& scene);

// The renderable content of a scene (US-076): the meshes, instances, lights, and suns that the digests of generate_workload cover,
// as plain values in metres and degrees. The renderer turns them into GPU objects; this file stays free of GPU types.
struct RenderMesh {
    std::vector<std::array<float, 3>> positions;  // metres, object space, in grid order
    std::vector<std::uint32_t> indices;           // three per triangle
};

struct RenderInstance {
    std::array<double, 3> position{};  // metres, world space; y is the terrain height at (x, z)
    std::uint32_t mesh = 0;            // index into RenderContent::meshes
    double yaw_deg = 0.0;
    double scale = 1.0;
};

struct RenderLight {
    std::uint32_t id = 0;  // 1-based, unique among the scene's lights
    std::array<double, 3> position{};
    std::array<float, 3> color{};  // 0..1 per channel
    double radius_m = 0.0;
    double intensity = 0.0;
    bool shadowed = false;
};

struct RenderSun {
    double azimuth_deg = 0.0;
    double elevation_deg = 0.0;
};

struct RenderContent {
    std::vector<RenderMesh> meshes;
    std::vector<RenderInstance> instances;
    std::vector<RenderLight> lights;
    std::vector<RenderSun> suns;
};

// Generates the renderable content of a scene from its table entry. The same functions produce the digests, so the content is the
// content the expected file describes. Fails under the same rules as generate_workload.
[[nodiscard]] core::Result<RenderContent, std::string> generate_render_content(const SceneDef& scene);

// Terrain height in millimetres at a world position (x, z in millimetres from the world origin). The terrain is a value
// noise on a lattice, sampled on the 1 m global grid, so neighbouring tiles agree on their shared edge by construction.
[[nodiscard]] std::int32_t terrain_height_mm(std::uint64_t world_seed, std::int64_t x_mm, std::int64_t z_mm);

// Returns one line per difference between an expected and an actual workload; empty when they agree.
[[nodiscard]] std::vector<std::string> compare_workloads(const Workload& expected, const Workload& actual);

// Expected-file format: {"workload_version": 1, "scenes": {"<name>": {"seed", "world_seed", "counts", "sha256"}}}.
[[nodiscard]] std::string workloads_json(const std::map<std::string, Workload>& scenes);
[[nodiscard]] core::Result<std::map<std::string, Workload>, std::string> parse_workloads(std::string_view text);

}  // namespace engine::bench
