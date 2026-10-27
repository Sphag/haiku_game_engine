// Native-profile GI probes (US-040), on the CPU. These cases bake an outdoor and an indoor fixture and check the indirect light they receive,
// the fallbacks for blocked, unbaked, and off-grid probes, the invalidation of stale data, the binary format, the coverage debug view, and
// the converter's gi_probes importer. The sky projection is checked against the environment's diffuse convention in pbr.cpp.

#include "asset_converter/importer.hpp"
#include "engine/assets/asset_registry.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/assets/cooked_container.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/gi/cooked_volume.hpp"
#include "engine/gi/probes.hpp"
#include "engine/render/pbr.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using engine::gi::Box;
using engine::gi::BakeInputs;
using engine::gi::bake_probes;
using engine::gi::bake_volume;
using engine::gi::coverage_debug_color;
using engine::gi::create_volume;
using engine::gi::deserialize;
using engine::gi::evaluate_irradiance;
using engine::gi::GiVolume;
using engine::gi::hash_inputs;
using engine::gi::is_current;
using engine::gi::Probe;
using engine::gi::ProbeState;
using engine::gi::project_sky;
using engine::gi::sample_irradiance;
using engine::gi::serialize;
using engine::gi::Sh9;
using engine::gi::Sky;
using engine::gi::validate_inputs;

namespace {

// Uniform sky: every direction sees the same radiance, so irradiance / pi equals the radiance.
Sky uniform_sky(float radiance) {
    Sky sky;
    sky.zenith = glm::vec3(radiance);
    sky.horizon = glm::vec3(radiance);
    sky.ground = glm::vec3(radiance);
    sky.sun_radiance = glm::vec3(0.0f);
    return sky;
}

// A 4 x 1 x 4 probe grid at height 1, two meters apart, from (1, 1, 1). It covers the room below and the outdoor ground around it.
BakeInputs grid_inputs(const Sky& sky) {
    BakeInputs inputs;
    inputs.grid.origin = glm::vec3(1.0f, 1.0f, 1.0f);
    inputs.grid.spacing = 2.0f;
    inputs.grid.dims = glm::uvec3(4u, 1u, 4u);
    inputs.sky = sky;
    inputs.samples = 256;
    return inputs;
}

// An 8 x 3 x 8 room of walls 0.2 m thick, with a roof and a 2 m wide window in the north wall. The probes sit in the room.
BakeInputs room_inputs(const Sky& sky) {
    BakeInputs inputs = grid_inputs(sky);
    const glm::vec3 albedo(0.6f);
    inputs.boxes = {
        {{0.0f, -0.2f, 0.0f}, {8.0f, 0.0f, 8.0f}, albedo, glm::vec3(0.0f)},     // floor
        {{0.0f, 3.0f, 0.0f}, {8.0f, 3.2f, 8.0f}, albedo, glm::vec3(0.0f)},      // roof
        {{-0.2f, 0.0f, 0.0f}, {0.0f, 3.0f, 8.0f}, albedo, glm::vec3(0.0f)},     // west wall
        {{8.0f, 0.0f, 0.0f}, {8.2f, 3.0f, 8.0f}, albedo, glm::vec3(0.0f)},      // east wall
        {{0.0f, 0.0f, -0.2f}, {8.0f, 3.0f, 0.0f}, albedo, glm::vec3(0.0f)},     // south wall
        {{0.0f, 0.0f, 8.0f}, {3.0f, 3.0f, 8.2f}, albedo, glm::vec3(0.0f)},      // north wall, west of the window
        {{5.0f, 0.0f, 8.0f}, {8.0f, 3.0f, 8.2f}, albedo, glm::vec3(0.0f)},      // north wall, east of the window
        {{3.0f, 2.2f, 8.0f}, {5.0f, 3.0f, 8.2f}, albedo, glm::vec3(0.0f)},      // lintel above the window
    };
    return inputs;
}

// The probe at grid index (x, 0, z) of the 4 x 1 x 4 grid.
glm::vec3 probe_world(std::uint32_t x, std::uint32_t z) {
    return glm::vec3(1.0f + 2.0f * static_cast<float>(x), 1.0f, 1.0f + 2.0f * static_cast<float>(z));
}

const Sh9 kBlack{};

}  // namespace

TEST_CASE("The sky projection reproduces a uniform sky in the environment's diffuse convention", "[unit]") {
    const Sh9 sky = project_sky(uniform_sky(0.5f));
    for (const glm::vec3& normal : {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::normalize(glm::vec3(1, 2, -3))}) {
        CHECK(evaluate_irradiance(sky, normal).x == Catch::Approx(0.5f).margin(0.01f));
    }

    // The same uniform radiance through build_environment() gives the same diffuse value, so the two conventions agree.
    engine::render::EquirectImage image;
    image.width = 64;
    image.height = 32;
    image.texels.assign(static_cast<std::size_t>(image.width) * image.height, glm::vec3(0.5f));
    const engine::render::Environment environment = engine::render::build_environment(image);
    const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, 0.8f, -0.5f));
    CHECK(evaluate_irradiance(sky, normal).x == Catch::Approx(engine::render::evaluate_diffuse(environment, normal).x).margin(0.01f));
}

TEST_CASE("Outdoor probes see the sky, and indoor probes get less light, most of it near the window", "[unit]") {
    Sky sky;
    sky.zenith = glm::vec3(0.35f, 0.55f, 0.95f);
    sky.horizon = glm::vec3(0.75f, 0.8f, 0.85f);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    const GiVolume outdoor = bake_volume(grid_inputs(sky));
    const GiVolume indoor = bake_volume(room_inputs(sky));

    const glm::vec3 centre_outdoor = sample_irradiance(outdoor, probe_world(1, 1), up, kBlack).irradiance;
    const glm::vec3 centre_indoor = sample_irradiance(indoor, probe_world(1, 1), up, kBlack).irradiance;
    const glm::vec3 near_window = sample_irradiance(indoor, probe_world(1, 3), up, kBlack).irradiance;
    const glm::vec3 far_corner = sample_irradiance(indoor, probe_world(0, 0), up, kBlack).irradiance;

    INFO("outdoor " << centre_outdoor.x << ", indoor centre " << centre_indoor.x << ", near window " << near_window.x << ", far corner "
                    << far_corner.x);
    CHECK(centre_outdoor.x > 0.0f);
    CHECK(centre_indoor.x > 0.0f);
    CHECK(centre_indoor.x < centre_outdoor.x);
    CHECK(near_window.x > far_corner.x);
    CHECK(far_corner.x > 0.0f);
    // Indirect light is not all the light: the bounce term lifts the indoor probes but never beyond the open sky.
    CHECK(near_window.x < centre_outdoor.x);
}

TEST_CASE("Probes inside geometry are blocked and take no weight from the lighting", "[unit]") {
    BakeInputs inputs = grid_inputs(uniform_sky(0.5f));
    inputs.boxes = {{{0.5f, 0.5f, 0.5f}, {1.5f, 1.5f, 1.5f}, glm::vec3(0.5f), glm::vec3(0.0f)}};  // covers probe (0, 0, 0)
    const GiVolume volume = bake_volume(inputs);
    CHECK(volume.probes[0].state == ProbeState::Blocked);
    CHECK(volume.probes[1].state == ProbeState::Valid);

    // The probe at index 0 is blocked, so a point halfway to its valid neighbour gets half the neighbour's value, at half coverage. The
    // neighbour is compared with itself rather than with the open sky, because the box shades part of its sky.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float neighbour = sample_irradiance(volume, probe_world(1, 0), up, kBlack).irradiance.x;
    const engine::gi::IrradianceSample sample = sample_irradiance(volume, probe_world(0, 0) + glm::vec3(1.0f, 0.0f, 0.0f), up, kBlack);
    CHECK(sample.coverage == Catch::Approx(0.5f).margin(1e-4f));
    CHECK(sample.irradiance.x == Catch::Approx(0.5f * neighbour).margin(1e-4f));
}

TEST_CASE("Empty, incomplete, and off-grid probes fall back to the environment, scaled by their coverage", "[unit]") {
    const BakeInputs inputs = grid_inputs(uniform_sky(0.5f));
    GiVolume volume = create_volume(inputs);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    // Nothing is baked: every lookup is the fallback, so the fallback environment (here black) is what comes back, with zero coverage.
    const engine::gi::IrradianceSample empty = sample_irradiance(volume, probe_world(1, 1), up, kBlack);
    CHECK(empty.coverage == 0.0f);
    CHECK(glm::length(empty.irradiance) == Catch::Approx(0.0f).margin(1e-6f));

    // A budgeted bake reaches the first three probes in index order: (0,0), (1,0), (2,0). The probe at (2,0) is baked, the one at (3,0) is not.
    CHECK(bake_probes(volume, inputs, 3) == 3);
    CHECK(volume.probes[2].state == ProbeState::Valid);
    CHECK(volume.probes[3].state == ProbeState::Unbaked);
    CHECK(sample_irradiance(volume, probe_world(2, 0), up, kBlack).coverage == Catch::Approx(1.0f));

    // Halfway between a baked probe and an unbaked one, coverage is one half and the rest of the answer is black.
    const engine::gi::IrradianceSample half = sample_irradiance(volume, probe_world(2, 0) + glm::vec3(1.0f, 0.0f, 0.0f), up, kBlack);
    CHECK(half.coverage == Catch::Approx(0.5f).margin(1e-4f));
    CHECK(half.irradiance.x == Catch::Approx(0.5f * 0.5f).margin(0.01f));

    // The rest of the grid resumes the bake, and a finished volume reports full coverage everywhere in the grid.
    CHECK(bake_probes(volume, inputs, 1000) == volume.probes.size() - 3);
    CHECK(sample_irradiance(volume, probe_world(3, 3), up, kBlack).coverage == Catch::Approx(1.0f));

    // Off the grid, the answer is the fallback alone.
    const engine::gi::IrradianceSample outside = sample_irradiance(volume, glm::vec3(-20.0f, 1.0f, 0.0f), up, kBlack);
    CHECK(outside.coverage == 0.0f);
    CHECK(glm::length(outside.irradiance) == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("Stale volumes are detected when any bake input changes, and a stale volume is never filled in", "[unit]") {
    const BakeInputs inputs = grid_inputs(uniform_sky(0.5f));
    GiVolume volume = create_volume(inputs);
    CHECK(is_current(volume, inputs));

    BakeInputs changed_box = inputs;
    changed_box.boxes = {{{10.0f, 0.0f, 0.0f}, {11.0f, 1.0f, 1.0f}, glm::vec3(0.5f), glm::vec3(0.0f)}};
    CHECK(hash_inputs(changed_box) != hash_inputs(inputs));
    CHECK_FALSE(is_current(volume, changed_box));
    CHECK(bake_probes(volume, changed_box, 16) == 0);
    CHECK(volume.probes[0].state == ProbeState::Unbaked);

    BakeInputs changed_sky = inputs;
    changed_sky.sky.zenith = glm::vec3(0.1f);
    CHECK(hash_inputs(changed_sky) != hash_inputs(inputs));

    BakeInputs changed_samples = inputs;
    changed_samples.samples = 64;
    CHECK(hash_inputs(changed_samples) != hash_inputs(inputs));
    CHECK(hash_inputs(inputs) == hash_inputs(grid_inputs(uniform_sky(0.5f))));
}

TEST_CASE("The serialized volume round-trips, and corrupt or foreign data is rejected with a reason", "[unit]") {
    const BakeInputs inputs = room_inputs(Sky{});
    const GiVolume volume = bake_volume(inputs);
    const std::string bytes = serialize(volume);

    const auto restored = deserialize(bytes);
    REQUIRE(restored.ok());
    CHECK(restored.value().source_hash == volume.source_hash);
    CHECK(restored.value().format_version == engine::gi::kFormatVersion);
    CHECK(restored.value().grid.dims == volume.grid.dims);
    REQUIRE(restored.value().probes.size() == volume.probes.size());
    for (std::size_t i = 0; i < volume.probes.size(); ++i) {
        CHECK(restored.value().probes[i].state == volume.probes[i].state);
        CHECK(restored.value().probes[i].sh == volume.probes[i].sh);
    }
    CHECK(is_current(restored.value(), inputs));

    CHECK_FALSE(deserialize(bytes.substr(0, bytes.size() - 4)).ok());
    CHECK_FALSE(deserialize(std::string("NOPE") + bytes.substr(4)).ok());

    std::string other_version = bytes;
    other_version[4] = static_cast<char>(engine::gi::kFormatVersion + 1);
    const auto rejected = deserialize(other_version);
    REQUIRE_FALSE(rejected.ok());
    CHECK(rejected.error().find("not supported") != std::string::npos);

    CHECK_FALSE(deserialize(bytes + "x").ok());
}

TEST_CASE("The coverage debug view colors each probe by its state, and marks positions off the grid", "[unit]") {
    BakeInputs inputs = grid_inputs(uniform_sky(0.5f));
    inputs.boxes = {{{0.5f, 0.5f, 0.5f}, {1.5f, 1.5f, 1.5f}, glm::vec3(0.5f), glm::vec3(0.0f)}};
    GiVolume volume = create_volume(inputs);
    CHECK(coverage_debug_color(volume, probe_world(1, 0)) == glm::vec3(1.0f, 1.0f, 0.0f));  // unbaked
    CHECK(bake_probes(volume, inputs, 2) == 2);
    CHECK(coverage_debug_color(volume, probe_world(0, 0)) == glm::vec3(1.0f, 0.0f, 0.0f));  // blocked
    CHECK(coverage_debug_color(volume, probe_world(1, 0)) == glm::vec3(0.0f, 1.0f, 0.0f));  // valid
    CHECK(coverage_debug_color(volume, glm::vec3(-50.0f)) == glm::vec3(0.1f));              // outside
}

TEST_CASE("Bake inputs that cannot be baked are rejected before any probe is computed", "[unit]") {
    CHECK_FALSE(validate_inputs(grid_inputs(Sky{})).has_value());

    BakeInputs no_probes = grid_inputs(Sky{});
    no_probes.grid.dims = glm::uvec3(0u, 1u, 1u);
    CHECK(validate_inputs(no_probes).has_value());

    BakeInputs too_many = grid_inputs(Sky{});
    too_many.grid.dims = glm::uvec3(4096u, 4096u, 4096u);
    CHECK(validate_inputs(too_many).has_value());

    BakeInputs zero_spacing = grid_inputs(Sky{});
    zero_spacing.grid.spacing = 0.0f;
    CHECK(validate_inputs(zero_spacing).has_value());

    BakeInputs inverted_box = grid_inputs(Sky{});
    inverted_box.boxes = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 1.0f}, glm::vec3(0.5f), glm::vec3(0.0f)}};
    CHECK(validate_inputs(inverted_box).has_value());

    BakeInputs no_samples = grid_inputs(Sky{});
    no_samples.samples = 0;
    CHECK(validate_inputs(no_samples).has_value());
}

TEST_CASE("The gi_probes importer bakes a JSON scene description and reports bad sources", "[unit]") {
    const engine::converter::ImporterRegistry registry = engine::converter::ImporterRegistry::builtin();
    const engine::converter::Importer* importer = registry.find("gi_probes");
    REQUIRE(importer != nullptr);
    CHECK(importer->canonical_settings("{}").ok());
    CHECK_FALSE(importer->canonical_settings("{\"samples\": 8}").ok());

    const std::string source = R"({
        "grid": {"origin": [1, 1, 1], "spacing": 2, "dims": [2, 1, 2]},
        "sky": {"zenith": [0.5, 0.5, 0.5], "horizon": [0.5, 0.5, 0.5], "ground": [0.5, 0.5, 0.5]},
        "occluders": [{"min": [10, 0, 10], "max": [11, 1, 11], "albedo": [0.5, 0.5, 0.5]}],
        "samples": 64
    })";
    const auto imported = importer->import(source, "{}");
    REQUIRE(imported.ok());
    const auto volume = deserialize(imported.value().payload);
    REQUIRE(volume.ok());
    CHECK(volume.value().probes.size() == 4);
    CHECK(volume.value().probes[0].state == ProbeState::Valid);
    CHECK(sample_irradiance(volume.value(), glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(0, 1, 0), kBlack).irradiance.x == Catch::Approx(0.5f).margin(0.01f));

    // Re-running the same source gives the same bytes, so the converter's output hashes are stable.
    CHECK(importer->import(source, "{}").value().payload == imported.value().payload);

    CHECK_FALSE(importer->import("{}", "{}").ok());
    CHECK_FALSE(importer->import(R"({"grid": {"spacing": 1, "dims": [-1, 1, 1]}, "occluders": []})", "{}").ok());
    CHECK_FALSE(importer->import(R"({"grid": {"spacing": 1, "dims": [1.5, 1, 1]}, "occluders": []})", "{}").ok());
    CHECK_FALSE(importer->import("not json", "{}").ok());
}

TEST_CASE("The runtime loads a cooked gi_probes asset through the registry, and rejects anything else", "[unit]") {
    const engine::converter::ImporterRegistry converters = engine::converter::ImporterRegistry::builtin();
    const engine::converter::Importer* importer = converters.find("gi_probes");
    REQUIRE(importer != nullptr);
    const std::string source = R"({
        "grid": {"origin": [1, 1, 1], "spacing": 2, "dims": [2, 1, 2]},
        "sky": {"zenith": [0.5, 0.5, 0.5], "horizon": [0.5, 0.5, 0.5], "ground": [0.5, 0.5, 0.5]},
        "occluders": [{"min": [10, 0, 10], "max": [11, 1, 11], "albedo": [0.5, 0.5, 0.5]}],
        "samples": 64
    })";
    const auto imported = importer->import(source, "{}");
    REQUIRE(imported.ok());
    const std::string payload = imported.value().payload;
    const auto expected = deserialize(payload);
    REQUIRE(expected.ok());

    // The converter writes "<output>/<guid>.cooked", the container around the payload.
    const engine::core::Guid guid = engine::core::Guid::generate();
    const std::filesystem::path output = std::filesystem::temp_directory_path() / ("engine_gi_cooked_" + guid.to_string());
    std::filesystem::create_directories(output);
    const std::string cooked = engine::assets::write_cooked_container(payload);
    {
        std::ofstream file(output / (guid.to_string() + ".cooked"), std::ios::binary);
        file << cooked;
    }

    const auto source_path = engine::core::Path::from_utf8("gi/room.json");
    const auto output_path = engine::core::Path::from_utf8(output.generic_string());
    REQUIRE(source_path.ok());
    REQUIRE(output_path.ok());

    engine::assets::AssetRegistry registry;
    REQUIRE(registry.add(engine::assets::AssetRecord{guid, source_path.value(), "gi_probes", "{}", engine::assets::ContentHash{}, {}}).ok());

    const auto loaded = engine::gi::load_gi_volume(registry, output_path.value(), source_path.value());
    REQUIRE(loaded.ok());
    CHECK(loaded.value().source_hash == expected.value().source_hash);
    REQUIRE(loaded.value().probes.size() == expected.value().probes.size());
    for (std::size_t i = 0; i < expected.value().probes.size(); ++i) {
        CHECK(loaded.value().probes[i].state == expected.value().probes[i].state);
        CHECK(loaded.value().probes[i].sh == expected.value().probes[i].sh);
    }

    // A raw payload without the container, and a truncated container, are rejected with a reason.
    CHECK_FALSE(engine::gi::decode_cooked_volume(payload).ok());
    CHECK_FALSE(engine::gi::decode_cooked_volume(cooked.substr(0, cooked.size() - 8)).ok());

    // An asset that is not a gi_probes asset, an unregistered path, and a missing cooked file all fail.
    const auto texture_path = engine::core::Path::from_utf8("textures/wall.png");
    REQUIRE(texture_path.ok());
    REQUIRE(registry.add(engine::assets::AssetRecord{engine::core::Guid::generate(), texture_path.value(), "texture", "{}", engine::assets::ContentHash{}, {}}).ok());
    const auto not_gi = engine::gi::load_gi_volume(registry, output_path.value(), texture_path.value());
    REQUIRE_FALSE(not_gi.ok());
    CHECK(not_gi.error().find("not gi_probes") != std::string::npos);

    const auto missing_record = engine::core::Path::from_utf8("gi/other.json");
    REQUIRE(missing_record.ok());
    CHECK_FALSE(engine::gi::load_gi_volume(registry, output_path.value(), missing_record.value()).ok());

    // A registered gi_probes asset that has not been cooked has no cooked file to read.
    const engine::core::Guid uncooked = engine::core::Guid::generate();
    const auto uncooked_path = engine::core::Path::from_utf8("gi/uncooked.json");
    REQUIRE(uncooked_path.ok());
    REQUIRE(registry.add(engine::assets::AssetRecord{uncooked, uncooked_path.value(), "gi_probes", "{}", engine::assets::ContentHash{}, {}}).ok());
    CHECK_FALSE(engine::gi::load_gi_volume(registry, output_path.value(), uncooked_path.value()).ok());

    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
}
