// GPU particles (US-051), CPU side: curves, emitter validation, the spawn accumulator, and the parameter block's offsets are pure functions and
// are tested directly. The system's argument checks run on the CPU reference device. That device keeps no buffer contents, so the system
// cannot be created on it (its counter and index buffers are uploaded), and the budget, pause, reset, and deletion are tested on the GPU
// in render_particles_gpu_tests.cpp.

#include "engine/render/particles.hpp"
#include "engine/core/coordinates.hpp"
#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/validation_device.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using engine::render::ParticleBlend;
using engine::render::ParticleBudget;
using engine::render::ParticleCurve;
using engine::render::ParticleFrame;
using engine::render::ParticleFrameState;
using engine::render::ParticleKey;
using engine::render::ParticleSystem;
using engine::render::EmitterDesc;
using engine::render::advance_spawn;
using engine::render::evaluate_curve;
using engine::render::kParticleParameterBytes;
using engine::render::pack_particle_parameters;
using engine::render::validate_curve;
using engine::render::validate_emitter;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::CommandList;
using engine::rhi::DescriptorSetLayoutHandle;
using engine::rhi::Format;
using engine::rhi::PipelineHandle;
using engine::rhi::QueueType;
using engine::rhi::ResourceState;
using engine::rhi::RhiErrorCode;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;
using engine::rhi::ValidationDevice;
using engine::rhi::ValidationDeviceDesc;

namespace {

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

template <typename T>
bool fails_with(const RhiResult<T>& result, RhiErrorCode code) {
    return !result.ok() && result.error().code == code;
}

float read_float(const std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t read_uint(const std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

ShaderBinary shader(ShaderStage stage, std::uint8_t tag) {
    return ShaderBinary{stage, "main", {tag}};
}

// Five shader entries with bytecode; the CPU device checks only that they are present.
struct Shaders {
    ShaderBinary vertex = shader(ShaderStage::Vertex, 1);
    ShaderBinary pixel = shader(ShaderStage::Pixel, 2);
    ShaderBinary simulate = shader(ShaderStage::Compute, 3);
    ShaderBinary args = shader(ShaderStage::Compute, 4);
};

EmitterDesc basic_emitter(std::uint32_t capacity) {
    EmitterDesc desc;
    desc.name = "test";
    desc.capacity = capacity;
    desc.spawn_rate = 4.0f;
    desc.lifetime_min = 1.0f;
    desc.lifetime_max = 1.0f;
    return desc;
}

}  // namespace

TEST_CASE("Curves validate their keys and interpolate between them", "[unit]") {
    ParticleCurve curve;
    curve.keys = {ParticleKey{0.0f, glm::vec4(0.0f)}, ParticleKey{0.5f, glm::vec4(1.0f)}, ParticleKey{1.0f, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)}};
    CHECK_FALSE(validate_curve(curve).has_value());

    // Between keys the value is linear; at a key it is the key's value; outside the keys it holds the end values.
    CHECK(evaluate_curve(curve, 0.25f).x == Catch::Approx(0.5f));
    CHECK(evaluate_curve(curve, 0.5f).x == Catch::Approx(1.0f));
    CHECK(evaluate_curve(curve, 0.75f).x == Catch::Approx(0.5f));
    CHECK(evaluate_curve(curve, 1.0f).w == Catch::Approx(1.0f));
    CHECK(evaluate_curve(curve, 2.0f).w == Catch::Approx(1.0f));

    ParticleCurve late;
    late.keys = {ParticleKey{0.5f, glm::vec4(2.0f)}};
    CHECK(evaluate_curve(late, 0.0f).x == Catch::Approx(2.0f));
    CHECK(evaluate_curve(late, 1.0f).x == Catch::Approx(2.0f));

    ParticleCurve empty;
    empty.keys.clear();
    CHECK(validate_curve(empty).has_value());

    ParticleCurve too_many;
    too_many.keys.assign(9, ParticleKey{0.0f, glm::vec4(1.0f)});
    CHECK(validate_curve(too_many).has_value());

    ParticleCurve unsorted;
    unsorted.keys = {ParticleKey{0.6f, glm::vec4(1.0f)}, ParticleKey{0.4f, glm::vec4(1.0f)}};
    CHECK(validate_curve(unsorted).has_value());

    ParticleCurve out_of_range;
    out_of_range.keys = {ParticleKey{1.5f, glm::vec4(1.0f)}};
    CHECK(validate_curve(out_of_range).has_value());

    ParticleCurve not_finite;
    not_finite.keys = {ParticleKey{0.0f, glm::vec4(std::numeric_limits<float>::quiet_NaN())}};
    CHECK(validate_curve(not_finite).has_value());
}

TEST_CASE("Emitter descriptions are checked before any resource exists", "[unit]") {
    CHECK_FALSE(validate_emitter(basic_emitter(16)).has_value());

    EmitterDesc no_capacity = basic_emitter(0);
    CHECK(validate_emitter(no_capacity).has_value());

    EmitterDesc no_direction = basic_emitter(16);
    no_direction.direction = glm::vec3(0.0f);
    CHECK(validate_emitter(no_direction).has_value());

    EmitterDesc zero_life = basic_emitter(16);
    zero_life.lifetime_min = 0.0f;
    CHECK(validate_emitter(zero_life).has_value());

    EmitterDesc life_order = basic_emitter(16);
    life_order.lifetime_min = 2.0f;
    life_order.lifetime_max = 1.0f;
    CHECK(validate_emitter(life_order).has_value());

    EmitterDesc wide_spread = basic_emitter(16);
    wide_spread.spread = 4.0f;
    CHECK(validate_emitter(wide_spread).has_value());

    EmitterDesc speed_order = basic_emitter(16);
    speed_order.speed_min = 3.0f;
    speed_order.speed_max = 2.0f;
    CHECK(validate_emitter(speed_order).has_value());

    EmitterDesc negative_rate = basic_emitter(16);
    negative_rate.spawn_rate = -1.0f;
    CHECK(validate_emitter(negative_rate).has_value());

    EmitterDesc inverted_bounds = basic_emitter(16);
    inverted_bounds.bounds_min = glm::vec3(1.0f);
    inverted_bounds.bounds_max = glm::vec3(0.0f);
    CHECK(validate_emitter(inverted_bounds).has_value());

    EmitterDesc bad_size = basic_emitter(16);
    bad_size.size.keys.clear();
    CHECK(validate_emitter(bad_size).has_value());
}

TEST_CASE("The spawn accumulator emits whole particles and clamps each frame to the capacity", "[unit]") {
    std::uint64_t dropped = 0;

    // Four per second at a quarter second a frame is exactly one particle per frame.
    double accumulator = 0.0;
    for (int frame = 0; frame < 8; ++frame) {
        CHECK(advance_spawn(accumulator, 4.0, 0.25, 64, dropped) == 1);
    }
    CHECK(dropped == 0);

    // Two per second at a quarter second is half a particle a frame: the spawns alternate 0, 1.
    accumulator = 0.0;
    std::uint32_t total = 0;
    for (int frame = 0; frame < 10; ++frame) {
        total += advance_spawn(accumulator, 2.0, 0.25, 64, dropped);
    }
    CHECK(total == 5);

    // A frame that owes more than the capacity spawns the capacity and counts the rest as dropped.
    accumulator = 0.0;
    dropped = 0;
    CHECK(advance_spawn(accumulator, 100.0, 1.0, 16, dropped) == 16);
    CHECK(dropped == 84);

    // A bad rate or step spawns nothing and leaves the state alone.
    accumulator = 0.0;
    CHECK(advance_spawn(accumulator, -4.0, 0.25, 16, dropped) == 0);
    CHECK(advance_spawn(accumulator, 4.0, std::numeric_limits<double>::quiet_NaN(), 16, dropped) == 0);
    CHECK(accumulator == 0.0);
}

TEST_CASE("The parameter block packs the emitter, curves, and spawn state at the shader's offsets", "[unit]") {
    EmitterDesc desc = basic_emitter(256);
    desc.direction = glm::vec3(0.0f, 3.0f, 0.0f);
    desc.spread = 0.5f;
    desc.position = glm::vec3(1.0f, 2.0f, 3.0f);
    desc.spawn_radius = 0.75f;
    desc.gravity = glm::vec3(0.0f, -2.0f, 0.0f);
    desc.blend = ParticleBlend::Additive;
    desc.seed = 42;
    desc.color.keys = {ParticleKey{0.0f, glm::vec4(1.0f)}, ParticleKey{1.0f, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)}};
    desc.size.keys = {ParticleKey{0.0f, glm::vec4(0.1f)}, ParticleKey{0.5f, glm::vec4(0.4f)}, ParticleKey{1.0f, glm::vec4(0.0f)}};

    ParticleFrameState state;
    state.spawn_count = 7;
    state.ring_head = 30;
    state.serial_base = 1000;
    state.kill_all = true;
    ParticleFrame frame;
    frame.dt = 0.5f;
    frame.view_projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    frame.camera_right = glm::vec3(1.0f, 0.0f, 0.0f);
    frame.camera_up = glm::vec3(0.0f, 0.0f, 1.0f);

    const std::vector<std::uint8_t> bytes = pack_particle_parameters(desc, state, frame, false);
    REQUIRE(bytes.size() == kParticleParameterBytes);
    CHECK(read_float(bytes, 0) == Catch::Approx(1.0f));
    CHECK(read_float(bytes, 12) == Catch::Approx(0.75f));
    CHECK(read_float(bytes, 16 + 4) == Catch::Approx(1.0f));  // the direction is normalised
    CHECK(read_float(bytes, 16 + 12) == Catch::Approx(std::cos(0.5f)));
    CHECK(read_float(bytes, 48 + 4) == Catch::Approx(-2.0f));
    CHECK(read_float(bytes, 96 + 4) == Catch::Approx(1.0f));  // second colour key time
    CHECK(read_float(bytes, 128 + 16) == Catch::Approx(0.0f));
    CHECK(read_float(bytes, 256 + 4) == Catch::Approx(0.5f));  // second size key time
    CHECK(read_float(bytes, 288 + 16) == Catch::Approx(0.4f));
    CHECK(read_uint(bytes, 416) == 2);
    CHECK(read_uint(bytes, 420) == 3);
    CHECK(read_uint(bytes, 424) == 1);
    CHECK(read_uint(bytes, 432) == 256);
    CHECK(read_uint(bytes, 436) == 7);
    CHECK(read_uint(bytes, 440) == 30);
    CHECK(read_uint(bytes, 444) == 1000);
    CHECK(read_float(bytes, 448) == Catch::Approx(0.5f));
    CHECK(read_uint(bytes, 464) == 1);
    CHECK(read_uint(bytes, 468) == 42);

    // The matrix goes in column by column; Vulkan gets the converted matrix.
    glm::mat4 stored;
    std::memcpy(&stored[0][0], bytes.data() + 512, sizeof(glm::mat4));
    CHECK(glm::all(glm::epsilonEqual(stored[1], frame.view_projection[1], 1e-6f)));
    const std::vector<std::uint8_t> vulkan_bytes = pack_particle_parameters(desc, state, frame, true);
    glm::mat4 vulkan_stored;
    std::memcpy(&vulkan_stored[0][0], vulkan_bytes.data() + 512, sizeof(glm::mat4));
    CHECK(glm::all(glm::epsilonEqual(vulkan_stored[1], engine::core::coords::to_vulkan_clip(frame.view_projection)[1], 1e-6f)));
}

TEST_CASE("A particle system needs all four shaders and a non-zero budget", "[unit]") {
    ValidationDeviceDesc device_desc;
    ValidationDevice device(device_desc);
    const Shaders shaders;
    ShaderBinary empty = shaders.args;
    empty.bytecode.clear();
    CHECK(fails_with(ParticleSystem::create(device, shaders.vertex, shaders.pixel, shaders.simulate, empty), RhiErrorCode::InvalidState));

    ParticleBudget no_particles;
    no_particles.max_particles = 0;
    CHECK(fails_with(ParticleSystem::create(device, shaders.vertex, shaders.pixel, shaders.simulate, shaders.args, no_particles),
                     RhiErrorCode::InvalidDescriptor));
}
