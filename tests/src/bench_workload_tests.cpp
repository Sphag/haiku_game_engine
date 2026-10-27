// Benchmark workloads (US-075): the seeded generator, its counts against the benchmark table, the expected file that pins each
// workload, and the refusals that keep a table from being generated quietly wrong. Everything runs from the repository files;
// no asset is downloaded and no private file is read.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace {

using engine::bench::compare_workloads;
using engine::bench::generate_workload;
using engine::bench::Manifest;
using engine::bench::SceneDef;
using engine::bench::Workload;

Manifest repository_manifest() {
    const auto path = engine::core::Path::from_utf8(ENGINE_TEST_BENCH_MANIFEST);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    const auto manifest = engine::bench::parse_manifest(text.value());
    REQUIRE(manifest.ok());
    return manifest.value();
}

std::map<std::string, Workload> expected_workloads() {
    const auto path = engine::core::Path::from_utf8(ENGINE_TEST_WORKLOAD_EXPECTED);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    const auto expected = engine::bench::parse_workloads(text.value());
    REQUIRE(expected.ok());
    return expected.value();
}

SceneDef scene_with(std::uint64_t seed, std::map<std::string, std::uint64_t> counts) {
    SceneDef scene;
    scene.seed = seed;
    scene.counts = std::move(counts);
    scene.route_status = "none";
    return scene;
}

// The scene the generator tests use when they need a small, valid workload.
SceneDef small_scene(std::uint64_t seed) {
    return scene_with(seed, {{"renderable_instances", 64}, {"unique_meshes", 4}, {"animated_characters", 2},
                             {"bones_per_character", 8}, {"live_gpu_particles", 2000}, {"local_lights", 6},
                             {"shadowed_local_lights", 2}, {"directional_suns", 1}});
}

}  // namespace

TEST_CASE("Workload: terrain heights are bounded integer millimetres that change smoothly", "[unit]") {
    using engine::bench::terrain_height_mm;
    const std::uint64_t seed = engine::bench::generator_world().world_seed;
    // The base is 40 m with at most 36 m of noise, so a height stays inside [4 m, 76 m].
    for (std::int64_t x = 0; x < 8192000; x += 40003) {
        for (std::int64_t z = 0; z < 8192000; z += 40009) {
            const std::int32_t height = terrain_height_mm(seed, x, z);
            REQUIRE(height >= 4000);
            REQUIRE(height <= 76000);
            // One metre along x changes the height by at most about 0.7 m: a 1 m sample grid can only be this smooth when the
            // lattice and interpolation match.
            const std::int32_t next = terrain_height_mm(seed, x + 1000, z);
            const std::int64_t step = static_cast<std::int64_t>(next) - height;
            REQUIRE(step <= 1000);
            REQUIRE(step >= -1000);
        }
    }
}

TEST_CASE("Workload: the terrain depends on the world seed", "[unit]") {
    using engine::bench::terrain_height_mm;
    std::int64_t differing = 0;
    for (std::int64_t x = 0; x < 64000; x += 1000) {
        if (terrain_height_mm(1, x, 3000) != terrain_height_mm(2, x, 3000)) {
            ++differing;
        }
    }
    CHECK(differing > 0);
}

TEST_CASE("Workload: the generator world matches the benchmark table's world block", "[unit]") {
    const Manifest manifest = repository_manifest();
    const auto check = engine::bench::check_generator_world(manifest.world_json);
    CHECK(check.ok());

    // A world block that disagrees with the generator is refused, not used.
    const auto wrong = engine::bench::check_generator_world(R"({"size_m": 4096, "cell_m": 256, "grid_cells": 16,
        "detail_window_cells": 9, "tile_height_samples": 257})");
    CHECK_FALSE(wrong.ok());
    CHECK(wrong.error().find("size_m") != std::string::npos);
}

TEST_CASE("Workload: every generated scene matches the expected file", "[unit]") {
    const Manifest manifest = repository_manifest();
    const auto expected = expected_workloads();
    std::size_t generated = 0;
    for (const auto& [name, scene] : manifest.scenes) {
        if (scene.builtin) {
            continue;
        }
        ++generated;
        INFO("scene " << name);
        REQUIRE(expected.count(name) == 1);
        const auto workload = generate_workload(name, scene);
        REQUIRE(workload.ok());
        const auto diffs = compare_workloads(expected.at(name), workload.value());
        for (const auto& diff : diffs) {
            INFO(diff);
        }
        CHECK(diffs.empty());
    }
    CHECK(generated == 4);
    CHECK(expected.size() == generated);
}

TEST_CASE("Workload: the counts follow the benchmark table", "[unit]") {
    const Manifest manifest = repository_manifest();

    const auto render = generate_workload("render_stress", manifest.scenes.at("render_stress"));
    REQUIRE(render.ok());
    CHECK(render.value().counts.at("instances") == 20000);
    CHECK(render.value().counts.at("meshes") == 200);
    CHECK(render.value().counts.at("characters") == 100);
    CHECK(render.value().counts.at("bones") == 6400);
    CHECK(render.value().counts.at("particles") == 100000);
    CHECK(render.value().counts.at("particle_emitters") == 100);
    CHECK(render.value().counts.at("lights") == 128);
    CHECK(render.value().counts.at("shadowed_lights") == 4);
    CHECK(render.value().counts.at("suns") == 1);

    const auto physics = generate_workload("physics_stress", manifest.scenes.at("physics_stress"));
    REQUIRE(physics.ok());
    CHECK(physics.value().counts.at("bodies") == 2000);
    CHECK(physics.value().counts.at("static_colliders") == 200);
    CHECK(physics.value().counts.at("constraints") == 100);
    CHECK(physics.value().counts.at("physics_characters") == 20);

    const auto streaming = generate_workload("streaming_stress", manifest.scenes.at("streaming_stress"));
    REQUIRE(streaming.ok());
    CHECK(streaming.value().counts.at("instances") == 200000);

    const auto all = generate_workload("all_features", manifest.scenes.at("all_features"));
    REQUIRE(all.ok());
    CHECK(all.value().counts.at("instances") == 100000);
    CHECK(all.value().counts.at("characters") == 50);
    CHECK(all.value().counts.at("bodies") == 1000);
    CHECK(all.value().counts.at("constraints") == 50);
    CHECK(all.value().counts.at("particles") == 50000);
    CHECK(all.value().counts.at("lights") == 64);
    CHECK(all.value().counts.at("audio_emitters") == 16);

    // The shared terrain window is the detail window (9 x 9 tiles), each tile with 257 x 257 samples.
    for (const auto* workload : {&render.value(), &physics.value(), &streaming.value(), &all.value()}) {
        CHECK(workload->counts.at("terrain_tiles") == 81);
        CHECK(workload->counts.at("terrain_samples") == 81ull * 257 * 257);
    }
}

TEST_CASE("Workload: a changed seed changes the content but not the shared terrain", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef base = manifest.scenes.at("render_stress");
    SceneDef moved = base;
    moved.seed = base.seed + 1;

    const auto first = generate_workload("render_stress", base);
    const auto second = generate_workload("render_stress", moved);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    // render_stress has no physics bodies or audio emitters, so those digests are the empty input and cannot change.
    for (const char* component : {"meshes", "instances", "characters", "lights", "particles"}) {
        INFO(component);
        CHECK(first.value().sha256.at(component) != second.value().sha256.at(component));
    }
    CHECK(first.value().sha256.at("terrain") == second.value().sha256.at("terrain"));
}

TEST_CASE("Workload: the same scene generates the same bytes twice", "[unit]") {
    const auto first = generate_workload("small", small_scene(42));
    const auto second = generate_workload("small", small_scene(42));
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(compare_workloads(first.value(), second.value()).empty());
}

TEST_CASE("Workload: the builtin calibration scene is not a generated workload", "[unit]") {
    SceneDef builtin = scene_with(1, {{"items", 4096}, {"rounds_per_frame", 32}});
    builtin.builtin = true;
    const auto result = generate_workload("calibration", builtin);
    CHECK_FALSE(result.ok());
}

TEST_CASE("Workload: a table key the generator does not know is refused", "[unit]") {
    const auto result = generate_workload("odd", scene_with(1, {{"renderable_instances", 10}, {"mystery_count", 3}}));
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("mystery_count") != std::string::npos);
}

TEST_CASE("Workload: runtime keys are accepted and not generated", "[unit]") {
    const auto result = generate_workload("route", scene_with(1, {{"renderable_instances", 10}, {"unique_meshes", 4},
                                                                  {"raycasts_per_s", 1000}, {"simulation_hz", 60}}));
    REQUIRE(result.ok());
    CHECK(result.value().counts.count("raycasts_per_s") == 0);
    CHECK(result.value().counts.at("instances") == 10);
}

TEST_CASE("Workload: impossible tables are refused, not generated", "[unit]") {
    SECTION("more shadowed lights than lights") {
        CHECK_FALSE(generate_workload("s", scene_with(1, {{"local_lights", 2}, {"shadowed_local_lights", 3}})).ok());
    }
    SECTION("particles that do not split into whole emitters") {
        CHECK_FALSE(generate_workload("s", scene_with(1, {{"live_gpu_particles", 1500}})).ok());
    }
    SECTION("characters without bones") {
        CHECK_FALSE(generate_workload("s", scene_with(1, {{"animated_characters", 3}})).ok());
    }
    SECTION("constraints with fewer than two bodies") {
        CHECK_FALSE(generate_workload("s", scene_with(1, {{"active_dynamic_bodies", 1}, {"constraints", 1}})).ok());
    }
    SECTION("instances without a mesh") {
        CHECK_FALSE(generate_workload("s", scene_with(1, {{"renderable_instances", 5}, {"unique_meshes", 0}})).ok());
    }
}

TEST_CASE("Workload: a digest that changes is reported by component", "[unit]") {
    const auto workload = generate_workload("small", small_scene(42));
    REQUIRE(workload.ok());
    Workload damaged = workload.value();
    damaged.counts["instances"] += 1;
    damaged.sha256["lights"] = std::string(64, '0');
    const auto diffs = compare_workloads(workload.value(), damaged);
    CHECK(diffs.size() == 2);
    bool names_count = false;
    bool names_digest = false;
    for (const auto& diff : diffs) {
        names_count = names_count || diff.find("count instances") != std::string::npos;
        names_digest = names_digest || diff.find("sha256 lights") != std::string::npos;
    }
    CHECK(names_count);
    CHECK(names_digest);
}

TEST_CASE("Workload: the expected file round-trips and refuses a different format version", "[unit]") {
    const auto workload = generate_workload("small", small_scene(42));
    REQUIRE(workload.ok());
    const std::map<std::string, Workload> scenes{{"small", workload.value()}};
    const std::string text = engine::bench::workloads_json(scenes);

    const auto parsed = engine::bench::parse_workloads(text);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().count("small") == 1);
    CHECK(compare_workloads(workload.value(), parsed.value().at("small")).empty());

    // The version is read from the constant, so a bump does not leave this test pointing at a stale number.
    const std::string current = "\"workload_version\": " + std::to_string(engine::bench::kWorkloadVersion);
    const std::string next = "\"workload_version\": " + std::to_string(engine::bench::kWorkloadVersion + 1);
    std::string other = text;
    const auto at = other.find(current);
    REQUIRE(at != std::string::npos);
    other.replace(at, current.size(), next);
    CHECK_FALSE(engine::bench::parse_workloads(other).ok());
    CHECK_FALSE(engine::bench::parse_workloads("not json").ok());
}

// The render content (US-076) is what render_stress draws, so it must have the table's counts and the instance triangles the
// generator reports. The route is the manifest's camera.
TEST_CASE("Workload: the render_stress content matches the table counts, the route, and the generated triangles", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("render_stress");
    REQUIRE(scene.camera.has_value());
    CHECK(scene.camera->start_x_m == 4096.0);
    CHECK(scene.camera->start_z_m == 4096.0);
    CHECK(scene.camera->heading_deg == 90.0);
    CHECK(scene.camera->speed_m_per_s == 2.0);
    CHECK(scene.route == "render_route_v1");

    const auto content = engine::bench::generate_render_content(scene);
    REQUIRE(content.ok());
    const auto& render = content.value();
    CHECK(render.meshes.size() == scene.counts.at("unique_meshes"));
    CHECK(render.instances.size() == scene.counts.at("renderable_instances"));
    CHECK(render.lights.size() == scene.counts.at("local_lights"));
    CHECK(render.suns.size() == scene.counts.at("directional_suns"));

    std::size_t shadowed = 0;
    for (const auto& light : render.lights) {
        shadowed += light.shadowed ? 1 : 0;
        CHECK(light.radius_m > 0.0);
    }
    CHECK(shadowed == scene.counts.at("shadowed_local_lights"));

    // Every instance names a mesh, and the instances stay in the 8192 m world.
    std::uint64_t instance_triangles = 0;
    for (const auto& instance : render.instances) {
        REQUIRE(instance.mesh < render.meshes.size());
        instance_triangles += render.meshes[instance.mesh].indices.size() / 3;
        CHECK(instance.position[0] >= 0.0);
        CHECK(instance.position[0] < 8192.0);
        CHECK(instance.position[2] < 8192.0);
        CHECK(instance.scale >= 0.8);
    }
    const auto workload = generate_workload("render_stress", scene);
    REQUIRE(workload.ok());
    CHECK(instance_triangles == workload.value().counts.at("instance_triangles"));
}
