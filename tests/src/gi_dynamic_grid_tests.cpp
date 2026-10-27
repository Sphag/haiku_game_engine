// Dynamic probe-grid GI bookkeeping (US-044), on the CPU. These cases check the description's limits, the camera-following toroidal cells,
// the round-robin update budget, the temporal blend weights, the history resets for teleports, streaming regions, and lighting changes, and
// the volume that the sampler reads. The GPU trace and its blend are checked by render_dynamic_gi_gpu_tests.cpp.

#include "engine/gi/dynamic_grid.hpp"
#include "engine/gi/probes.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using engine::gi::DynamicGrid;
using engine::gi::DynamicGridDesc;
using engine::gi::DynamicLighting;
using engine::gi::ProbeUpdate;
using engine::gi::ProbeState;
using engine::gi::Sh9;
using engine::gi::kMaxDynamicProbes;
using engine::gi::kMaxDynamicRays;
using engine::gi::sample_irradiance;
using engine::gi::validate_desc;

namespace {

// An 8 x 4 x 8 grid of 2 m probes: 256 slots. The budget is set per test.
DynamicGridDesc small_desc(std::uint32_t budget) {
    DynamicGridDesc desc;
    desc.dims = glm::uvec3(8u, 4u, 8u);
    desc.spacing = 2.0f;
    desc.rays_per_probe = 16;
    desc.update_budget = budget;
    desc.max_history = 4;
    desc.lighting_history = 2;
    return desc;
}

DynamicGrid make_grid(std::uint32_t budget) {
    auto created = DynamicGrid::create(small_desc(budget));
    REQUIRE(created.ok());
    return std::move(created).value();
}

// Plans and commits one frame, the way the render pass does.
std::vector<ProbeUpdate> run_frame(DynamicGrid& grid) {
    std::vector<ProbeUpdate> plan = grid.plan_updates();
    grid.commit(plan);
    return plan;
}

// The weight that a plan gives one slot, or a negative value when the slot is not in the plan.
float weight_of(const std::vector<ProbeUpdate>& plan, std::uint32_t slot) {
    for (const ProbeUpdate& update : plan) {
        if (update.slot == slot) {
            return update.weight;
        }
    }
    return -1.0f;
}

Sh9 constant_sh(const glm::vec3& value) {
    Sh9 sh{};
    sh[0] = value;
    return sh;
}

}  // namespace

TEST_CASE("The dynamic grid description states its limits, and an invalid one builds no grid", "[unit]") {
    CHECK_FALSE(validate_desc(small_desc(32)).has_value());

    DynamicGridDesc empty = small_desc(32);
    empty.dims.y = 0;
    CHECK(validate_desc(empty).has_value());

    DynamicGridDesc too_many = small_desc(32);
    too_many.dims = glm::uvec3(64u, 64u, 64u);
    REQUIRE(static_cast<std::size_t>(64) * 64 * 64 > kMaxDynamicProbes);
    CHECK(validate_desc(too_many).has_value());

    DynamicGridDesc spacing = small_desc(32);
    spacing.spacing = std::numeric_limits<float>::quiet_NaN();
    CHECK(validate_desc(spacing).has_value());
    spacing.spacing = 0.0f;
    CHECK(validate_desc(spacing).has_value());

    DynamicGridDesc rays = small_desc(32);
    rays.rays_per_probe = 0;
    CHECK(validate_desc(rays).has_value());
    rays.rays_per_probe = kMaxDynamicRays + 1;
    CHECK(validate_desc(rays).has_value());

    CHECK(validate_desc(small_desc(0)).has_value());
    CHECK(validate_desc(small_desc(257)).has_value());
    CHECK_FALSE(validate_desc(small_desc(256)).has_value());

    DynamicGridDesc history = small_desc(32);
    history.max_history = 0;
    CHECK(validate_desc(history).has_value());
    history.max_history = 4;
    history.lighting_history = 0;
    CHECK(validate_desc(history).has_value());
    history.lighting_history = 5;
    CHECK(validate_desc(history).has_value());

    CHECK(DynamicGrid::create(small_desc(32)).ok());
    CHECK_FALSE(DynamicGrid::create(small_desc(0)).ok());
}

TEST_CASE("The grid starts centred on the origin, and each slot holds the cell of its residue", "[unit]") {
    DynamicGrid grid = make_grid(32);
    REQUIRE(grid.probe_count() == 256);

    // With the eye at the origin the grid covers cells [-4, 4) x [-2, 2) x [-4, 4). A slot holds the cell in that window with the same
    // residue, so slot 0 holds cell 0 and slot 5 (residue 5 mod 8 on x) holds cell -3.
    CHECK(grid.follow(glm::vec3(0.0f)) == 0);
    CHECK(grid.cell(0) == glm::ivec3(0, 0, 0));
    CHECK(grid.cell(5) == glm::ivec3(-3, 0, 0));
    CHECK(grid.probe_position(0) == glm::vec3(1.0f, 1.0f, 1.0f));
}

TEST_CASE("Following the camera resets only the plane of cells that enters the grid", "[unit]") {
    DynamicGrid grid = make_grid(32);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    // Eight frames of 32 probes cover all 256 slots once, so every probe holds one sample.
    for (int frame = 0; frame < 8; ++frame) {
        static_cast<void>(run_frame(grid));
    }
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        REQUIRE(grid.samples(slot) == 1);
    }

    // A move of one cell along +x brings in one plane: 4 x 8 = 32 probes. The eye at x = 2 is cell 1.
    CHECK(grid.follow(glm::vec3(2.0f, 0.0f, 0.0f)) == 32);
    std::size_t reset = 0;
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        if (grid.samples(slot) == 0) {
            ++reset;
            CHECK(grid.cell(slot).x == 4);  // the new plane's cell
        }
    }
    CHECK(reset == 32);

    // A move within the same cell changes nothing.
    CHECK(grid.follow(glm::vec3(2.5f, 0.0f, 0.0f)) == 0);
}

TEST_CASE("A teleport resets every probe and keeps the grid where it is", "[unit]") {
    DynamicGrid grid = make_grid(256);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    static_cast<void>(run_frame(grid));
    const glm::ivec3 before = grid.cell(0);

    grid.reset();
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        REQUIRE(grid.samples(slot) == 0);
    }
    CHECK(grid.cell(0) == before);
}

TEST_CASE("A streaming region resets the probes inside it and nothing else", "[unit]") {
    DynamicGrid grid = make_grid(256);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    static_cast<void>(run_frame(grid));

    const glm::vec3 min(-1.0f);
    const glm::vec3 max(1.0f);
    std::size_t inside = 0;
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        const glm::vec3 position = grid.probe_position(slot);
        const bool in = glm::all(glm::greaterThanEqual(position, min)) && glm::all(glm::lessThanEqual(position, max));
        if (in) {
            ++inside;
        }
    }
    REQUIRE(inside == 8);
    CHECK(grid.reset_region(min, max) == inside);
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        const glm::vec3 position = grid.probe_position(slot);
        const bool in = glm::all(glm::greaterThanEqual(position, min)) && glm::all(glm::lessThanEqual(position, max));
        CHECK(grid.samples(slot) == (in ? 0u : 1u));
    }
}

TEST_CASE("The update budget visits every probe once per cycle of the grid, in round-robin order", "[unit]") {
    DynamicGrid grid = make_grid(32);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));

    std::set<std::uint32_t> visited;
    for (int frame = 0; frame < 8; ++frame) {
        const std::vector<ProbeUpdate> plan = run_frame(grid);
        REQUIRE(plan.size() == 32);
        for (std::size_t i = 0; i < plan.size(); ++i) {
            CHECK(plan[i].slot == static_cast<std::uint32_t>(frame * 32 + static_cast<int>(i)));
            visited.insert(plan[i].slot);
        }
    }
    CHECK(visited.size() == grid.probe_count());
}

TEST_CASE("The blend weight is one for a fresh probe, then falls to the history floor", "[unit]") {
    DynamicGrid grid = make_grid(256);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));

    // max_history is 4, so the weights are 1, 1/2, 1/3, 1/4, and then the floor 1/4 stays.
    const float expected[] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 0.25f, 0.25f};
    for (const float weight : expected) {
        const std::vector<ProbeUpdate> plan = grid.plan_updates();
        CHECK(weight_of(plan, 0) == Catch::Approx(weight));
        grid.commit(plan);
    }
    CHECK(grid.samples(0) == 4);
}

TEST_CASE("A commit does not count a trace for a probe that moved since it was planned", "[unit]") {
    DynamicGrid grid = make_grid(256);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    const std::vector<ProbeUpdate> plan = grid.plan_updates();

    // Eight cells along x is the grid's width, so every slot's cell changes.
    CHECK(grid.follow(glm::vec3(16.0f, 0.0f, 0.0f)) == grid.probe_count());
    grid.commit(plan);
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        REQUIRE(grid.samples(slot) == 0);
    }
}

TEST_CASE("A lighting change keeps lighting_history samples, and the same lighting changes nothing", "[unit]") {
    DynamicGrid grid = make_grid(256);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    CHECK_FALSE(grid.set_lighting(DynamicLighting{}));
    for (int frame = 0; frame < 4; ++frame) {
        static_cast<void>(run_frame(grid));
    }
    REQUIRE(grid.samples(0) == 4);

    DynamicLighting sunset;
    sunset.sun_direction = glm::normalize(glm::vec3(1.0f, 0.2f, 0.0f));
    sunset.sun_radiance = glm::vec3(2.0f, 1.5f, 1.0f);
    sunset.sky_radiance = glm::vec3(0.2f);
    CHECK(grid.set_lighting(sunset));
    CHECK(grid.samples(0) == 2);
    CHECK_FALSE(grid.set_lighting(sunset));
    CHECK(grid.samples(0) == 2);

    // The next trace averages with weight 1/3, so the old light fades within a few frames.
    CHECK(weight_of(grid.plan_updates(), 0) == Catch::Approx(1.0f / 3.0f));
}

TEST_CASE("The volume places each slot at its cell, and unsampled probes take no weight", "[unit]") {
    engine::gi::DynamicGridDesc desc;
    desc.dims = glm::uvec3(2u, 2u, 2u);
    desc.spacing = 2.0f;
    desc.rays_per_probe = 1;
    desc.update_budget = 8;
    auto created = DynamicGrid::create(desc);
    REQUIRE(created.ok());
    DynamicGrid grid = std::move(created).value();
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    static_cast<void>(run_frame(grid));

    // The window is cells [-1, 1) on each axis, so its first cell (-1, -1, -1) has residue (1, 1, 1): slot 1 + 2 * (1 + 2 * 1) = 7.
    std::vector<Sh9> history;
    for (std::uint32_t slot = 0; slot < grid.probe_count(); ++slot) {
        history.push_back(constant_sh(glm::vec3(static_cast<float>(slot) + 1.0f)));
    }
    const engine::gi::GiVolume volume = grid.to_volume(history);
    REQUIRE(volume.probes.size() == 8);
    CHECK(volume.grid.origin == glm::vec3(-1.0f));
    CHECK(volume.grid.spacing == 2.0f);
    CHECK(volume.probes[0].sh[0] == glm::vec3(8.0f));  // slot 7, stored as slot + 1
    for (const auto& probe : volume.probes) {
        CHECK(probe.state == ProbeState::Valid);
    }

    // Every probe is valid, so the sample is finite, fully covered, and equal to the constant history.
    const engine::gi::IrradianceSample sampled = sample_irradiance(volume, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), Sh9{});
    CHECK(sampled.coverage == Catch::Approx(1.0f));
    CHECK(std::isfinite(sampled.irradiance.x));
    CHECK(std::isfinite(sampled.irradiance.y));
    CHECK(std::isfinite(sampled.irradiance.z));

    // After a teleport no probe has a sample, so the sampler falls back entirely.
    grid.reset();
    const engine::gi::GiVolume cleared = grid.to_volume(history);
    const engine::gi::IrradianceSample none = sample_irradiance(cleared, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), Sh9{});
    CHECK(none.coverage == 0.0f);

    // A history of the wrong size gives an all-unbaked volume rather than reading past it.
    const engine::gi::GiVolume wrong = grid.to_volume(std::vector<Sh9>(3));
    for (const auto& probe : wrong.probes) {
        CHECK(probe.state == ProbeState::Unbaked);
    }
}

TEST_CASE("A non-finite eye leaves the grid where it is", "[unit]") {
    DynamicGrid grid = make_grid(32);
    static_cast<void>(grid.follow(glm::vec3(0.0f)));
    const glm::ivec3 before = grid.cell(0);
    CHECK(grid.follow(glm::vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f)) == 0);
    CHECK(grid.cell(0) == before);
}
