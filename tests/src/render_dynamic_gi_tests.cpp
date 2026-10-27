// The dynamic GI pass's CPU contract (US-044): the full profile's grid numbers, the byte images the shader reads, and the record strides.
// These cases need no device. The GPU trace and blend are checked by render_dynamic_gi_gpu_tests.cpp.

#include "engine/gi/dynamic_grid.hpp"
#include "engine/render/dynamic_gi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using engine::gi::DynamicGridDesc;
using engine::gi::DynamicLighting;
using engine::gi::ProbeUpdate;
using engine::render::DynamicGiSettings;
using engine::render::full_profile_grid;
using engine::render::kDynamicGiParameterBytes;
using engine::render::kDynamicGiProbeStride;
using engine::render::kDynamicGiSamplingHeaderBytes;
using engine::render::kDynamicGiUpdateStride;
using engine::render::pack_dynamic_parameters;
using engine::render::pack_dynamic_sampling;
using engine::render::pack_dynamic_updates;

namespace {

template <typename T>
T read_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

}  // namespace

TEST_CASE("The full profile's dynamic GI matches the PRD's grid, spacing, rays, and one-sixteenth update", "[unit]") {
    const DynamicGridDesc desc = full_profile_grid();
    CHECK(desc.dims == glm::uvec3(32u, 8u, 32u));
    CHECK(desc.spacing == 4.0f);
    CHECK(desc.rays_per_probe == 64u);
    const std::uint32_t probes = desc.dims.x * desc.dims.y * desc.dims.z;
    CHECK(probes == 8192u);
    CHECK(desc.update_budget * 16u == probes);
    CHECK_FALSE(engine::gi::validate_desc(desc).has_value());
}

TEST_CASE("The parameter block and the update records use the offsets of the shader", "[unit]") {
    DynamicGridDesc grid = full_profile_grid();
    grid.spacing = 2.5f;
    grid.rays_per_probe = 48;
    DynamicLighting lighting;
    lighting.sun_direction = glm::vec3(0.0f, 0.6f, 0.8f);
    lighting.sun_radiance = glm::vec3(1.0f, 0.9f, 0.8f);
    lighting.sky_radiance = glm::vec3(0.1f, 0.2f, 0.3f);
    DynamicGiSettings settings;
    settings.shadow_distance = 250.0f;
    settings.bias = 0.05f;

    const std::vector<std::uint8_t> params = pack_dynamic_parameters(grid, lighting, settings, 512);
    REQUIRE(params.size() == kDynamicGiParameterBytes);
    CHECK(read_at<float>(params, 0) == 2.5f);
    CHECK(read_at<std::uint32_t>(params, 4) == 48u);
    CHECK(read_at<std::uint32_t>(params, 8) == 512u);
    CHECK(read_at<float>(params, 12) == 250.0f);
    CHECK(read_at<glm::vec3>(params, 16) == lighting.sun_direction);
    CHECK(read_at<glm::vec3>(params, 32) == lighting.sun_radiance);
    CHECK(read_at<glm::vec3>(params, 48) == lighting.sky_radiance);
    CHECK(read_at<float>(params, 60) == 0.05f);
    CHECK(read_at<glm::vec3>(params, 64) == glm::vec3(0.0f));  // the camera's world position defaults to the origin
}

TEST_CASE("The parameter block carries the camera's world position, which the traced scene is relative to", "[unit]") {
    const glm::vec3 camera(1234.5f, 2.0f, -987.25f);
    const std::vector<std::uint8_t> params = pack_dynamic_parameters(full_profile_grid(), DynamicLighting{}, DynamicGiSettings{}, 1, camera);
    REQUIRE(params.size() == 80u);
    CHECK(read_at<glm::vec3>(params, 64) == camera);
}

TEST_CASE("The sampling block holds the window, the residue of its origin, and one valid flag per slot", "[unit]") {
    DynamicGridDesc desc = full_profile_grid();
    desc.dims = glm::uvec3(4u, 2u, 4u);
    desc.update_budget = 8;
    auto grid = engine::gi::DynamicGrid::create(desc);
    REQUIRE(grid.ok());
    engine::gi::DynamicGrid& g = grid.value();
    // Negative cells: the residue must be the positive modulus, not the C++ remainder.
    g.follow(glm::vec3(-41.0f, 0.0f, 9.0f));
    const std::vector<ProbeUpdate> plan = g.plan_updates();
    g.commit(plan);

    const std::vector<std::uint8_t> bytes = pack_dynamic_sampling(g);
    REQUIRE(bytes.size() == kDynamicGiSamplingHeaderBytes + g.probe_count() * 4u);
    const glm::ivec3 origin = g.origin();
    CHECK(read_at<glm::ivec3>(bytes, 0) == origin);
    CHECK(read_at<float>(bytes, 12) == desc.spacing);
    CHECK(read_at<glm::uvec3>(bytes, 16) == desc.dims);
    CHECK(read_at<std::uint32_t>(bytes, 28) == 1u);
    for (int axis = 0; axis < 3; ++axis) {
        const auto dim = static_cast<std::int32_t>(desc.dims[axis]);
        const auto residue = static_cast<std::uint32_t>(((origin[axis] % dim) + dim) % dim);
        CHECK(read_at<std::uint32_t>(bytes, 32 + 4 * static_cast<std::size_t>(axis)) == residue);
    }
    // A slot is valid exactly when it holds a trace. The plan traced the first update_budget slots in round-robin order.
    std::uint32_t valid = 0;
    for (std::uint32_t slot = 0; slot < g.probe_count(); ++slot) {
        const std::uint32_t flag = read_at<std::uint32_t>(bytes, kDynamicGiSamplingHeaderBytes + 4 * static_cast<std::size_t>(slot));
        CHECK(flag == (g.samples(slot) > 0 ? 1u : 0u));
        valid += flag;
    }
    CHECK(valid == desc.update_budget);
}

TEST_CASE("The plan's records hold the slot, the weight, and the cell in the stride of the shader", "[unit]") {
    std::vector<ProbeUpdate> plan(2);
    plan[0].slot = 7;
    plan[0].weight = 0.25f;
    plan[0].cell = glm::ivec3(-3, 0, 5);
    plan[1].slot = 300;
    plan[1].weight = 1.0f;
    plan[1].cell = glm::ivec3(4, -2, -1);

    const std::vector<std::uint8_t> bytes = pack_dynamic_updates(plan);
    REQUIRE(bytes.size() == 2 * kDynamicGiUpdateStride);
    CHECK(kDynamicGiUpdateStride == 32u);
    CHECK(read_at<std::uint32_t>(bytes, 0) == 7u);
    CHECK(read_at<float>(bytes, 4) == 0.25f);
    CHECK(read_at<glm::ivec3>(bytes, 8) == glm::ivec3(-3, 0, 5));
    CHECK(read_at<std::uint32_t>(bytes, 32) == 300u);
    CHECK(read_at<float>(bytes, 36) == 1.0f);
    CHECK(read_at<glm::ivec3>(bytes, 40) == glm::ivec3(4, -2, -1));
}

TEST_CASE("A probe's history is nine float4 coefficients", "[unit]") {
    CHECK(kDynamicGiProbeStride == 9u * 16u);
}
