// GPU particles (US-051) on real GPUs. Each case runs ParticleSystem on DX12 or Vulkan with the debug layers on, and any validation error fails
// it. The cases check the live count against the spawn schedule, reproduce a seeded emitter bit for bit on a second device, draw the billboards
// into an HDR target through the alpha and additive blends and against the scene depth, and check the budget, pause, reset, and deletion.
#include "engine/core/coordinates.hpp"
#include "engine/render/particles.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::render::EmitterDesc;
using engine::render::ParticleBlend;
using engine::render::ParticleBudget;
using engine::render::ParticleEmitterId;
using engine::render::ParticleFrame;
using engine::render::ParticleSystem;
using engine::render::ParticleTargets;
using engine::render::kParticleStrideBytes;
using engine::rhi::Backend;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::CommandList;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::FenceHandle;
using engine::rhi::Format;
using engine::rhi::QueueType;
using engine::rhi::RenderPassDesc;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;


namespace {

constexpr std::uint32_t kImage = 64;
constexpr std::uint32_t kCapacity = 64;

template <typename T>
bool refused(const RhiResult<T>& result, engine::rhi::RhiErrorCode code) {
    return !result.ok() && result.error().code == code;
}

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

void require_ok(const RhiStatus& status) {
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    return desc;
}

std::vector<std::uint8_t> load_shader(Backend backend, const std::string& name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + name + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The four entries of particles.hlsl, with their entry names as the HLSL declares them.
struct Shaders {
    ShaderBinary vertex;
    ShaderBinary pixel;
    ShaderBinary simulate;
    ShaderBinary args;
};

Shaders load_shaders(Backend backend) {
    return Shaders{ShaderBinary{ShaderStage::Vertex, "VSParticle", load_shader(backend, "particles_vertex")},
                   ShaderBinary{ShaderStage::Pixel, "PSParticle", load_shader(backend, "particles_pixel")},
                   ShaderBinary{ShaderStage::Compute, "CSParticleSimulate", load_shader(backend, "particles_simulate")},
                   ShaderBinary{ShaderStage::Compute, "CSParticleArgs", load_shader(backend, "particles_args")}};
}

// A device, its fence, and the staging buffers of the uploads, which are destroyed after each wait.
struct Gpu {
    std::unique_ptr<Device> device;
    FenceHandle fence;
    std::uint64_t value = 0;
};

Gpu make_gpu(Backend backend) {
    Gpu gpu;
    gpu.device = take(engine::rhi::create_device(debug_device(backend)));
    gpu.fence = take(gpu.device->create_fence(0));
    return gpu;
}

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

void submit_and_wait(Gpu& gpu, CommandList& list) {
    require_ok(list.close());
    std::vector<CommandList*> lists{&list};
    require_ok(gpu.device->submit(QueueType::Graphics, lists, gpu.fence, ++gpu.value));
    require_ok(gpu.device->wait_fence(gpu.fence, gpu.value));
}

// A 4 x 4 white RGBA8 sprite, uploaded row by row at the copy pitch, and left in ShaderRead.
TextureHandle make_sprite(Gpu& gpu) {
    constexpr std::uint32_t kSize = 4;
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(kSize, Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(pitch * kSize, 0);
    for (std::uint32_t row = 0; row < kSize; ++row) {
        std::memset(bytes.data() + pitch * row, 0xFF, kSize * 4);
    }
    BufferHandle staging = take(gpu.device->create_buffer(buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "sprite staging")));
    require_ok(gpu.device->upload_buffer(staging, 0, bytes.size(), bytes.data()));
    TextureDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.format = Format::R8G8B8A8Unorm;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDestination;
    desc.debug_name = "particle sprite";
    TextureHandle sprite = take(gpu.device->create_texture(desc));
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(sprite, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list->copy_buffer_to_texture(staging, 0, sprite));
    require_ok(list->transition(sprite, ResourceState::CopyDestination, ResourceState::ShaderRead));
    submit_and_wait(gpu, *list);
    require_ok(gpu.device->destroy(staging));
    return sprite;
}

// Copies a buffer out through a readback buffer. The buffer is returned to `state`.
std::vector<std::uint8_t> read_buffer(Gpu& gpu, BufferHandle buffer, ResourceState state, std::uint64_t size) {
    BufferHandle readback = take(gpu.device->create_buffer(buffer_desc(size, BufferUsage::CpuRead | BufferUsage::CopyDestination, "particle readback")));
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(buffer, state, ResourceState::CopySource));
    require_ok(list->copy_buffer(buffer, 0, readback, 0, size));
    require_ok(list->transition(buffer, ResourceState::CopySource, state));
    submit_and_wait(gpu, *list);
    std::vector<std::uint8_t> bytes(size);
    require_ok(gpu.device->read_buffer(readback, 0, size, bytes.data()));
    require_ok(gpu.device->destroy(readback));
    return bytes;
}

std::uint32_t read_uint(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

float half_to_float(std::uint16_t half) {
    const int sign = (half >> 15) & 1;
    const int exponent = (half >> 10) & 0x1f;
    const int mantissa = half & 0x3ff;
    float magnitude = 0.0f;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    } else {
        magnitude = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, exponent - 15);
    }
    return sign != 0 ? -magnitude : magnitude;
}

// RGBA of the HDR target at (x, y), from the R16G16B16A16F rows the readback returns at the copy pitch.
glm::vec4 pixel(const std::vector<std::uint8_t>& rows, std::uint32_t x, std::uint32_t y) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(kImage, Format::R16G16B16A16Float);
    glm::vec4 value(0.0f);
    for (int c = 0; c < 4; ++c) {
        std::uint16_t half = 0;
        std::memcpy(&half, rows.data() + pitch * y + 8 * x + 2 * static_cast<std::size_t>(c), sizeof(half));
        value[c] = half_to_float(half);
    }
    return value;
}

// The camera at the origin looking down -Z, a 60 degree field, and the view-projection in the engine's clip convention.
ParticleFrame camera_frame(float dt) {
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    ParticleFrame frame;
    frame.view_projection = engine::core::coords::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) * view;
    frame.camera_right = glm::vec3(1.0f, 0.0f, 0.0f);
    frame.camera_up = glm::vec3(0.0f, 1.0f, 0.0f);
    frame.dt = dt;
    return frame;
}

// Four particles a second over a quarter-second frame is one spawn a frame. Lifetime one second: a particle lives four frames.
EmitterDesc steady_emitter(std::uint32_t capacity) {
    EmitterDesc desc;
    desc.name = "steady";
    desc.position = glm::vec3(0.0f, 0.0f, -5.0f);
    desc.spawn_rate = 4.0f;
    desc.lifetime_min = 1.0f;
    desc.lifetime_max = 1.0f;
    desc.gravity = glm::vec3(0.0f);
    desc.size.keys = {engine::render::ParticleKey{0.0f, glm::vec4(2.0f)}};
    desc.capacity = capacity;
    return desc;
}

// Frame records: prepare on the CPU, then one list that simulates and submits.
void run_frame(Gpu& gpu, ParticleSystem& system, const ParticleFrame& frame) {
    require_ok(system.prepare(frame));
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(system.simulate(*list));
    submit_and_wait(gpu, *list);
}

// The bytes of an emitter's particle pool and its live count after a readback.
struct PoolState {
    std::vector<std::uint8_t> pool;
    std::uint32_t live = 0;
};

PoolState read_pool(Gpu& gpu, ParticleSystem& system, ParticleEmitterId id, std::uint32_t capacity) {
    PoolState state;
    state.pool = read_buffer(gpu, system.particle_pool(id), ResourceState::ShaderRead, capacity * kParticleStrideBytes);
    state.live = read_uint(read_buffer(gpu, system.live_counter(id), ResourceState::ShaderRead, 16), 0);
    return state;
}

// Frames of a seeded emitter with spread, speed, gravity, and a spawn radius: every field of the particle record depends on the seed.
PoolState run_seeded(Backend backend, std::uint32_t seed) {
    Gpu gpu = make_gpu(backend);
    const Shaders shaders = load_shaders(backend);
    auto system = take(ParticleSystem::create(*gpu.device, shaders.vertex, shaders.pixel, shaders.simulate, shaders.args));
    EmitterDesc desc = steady_emitter(kCapacity);
    desc.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    desc.spread = 0.7f;
    desc.speed_min = 0.5f;
    desc.speed_max = 2.0f;
    desc.spawn_radius = 0.6f;
    desc.gravity = glm::vec3(0.0f, -2.0f, 0.0f);
    desc.spawn_rate = 16.0f;
    desc.seed = seed;
    const ParticleEmitterId emitter = take(system->add_emitter(desc));
    for (int frame = 0; frame < 6; ++frame) {
        run_frame(gpu, *system, camera_frame(0.25f));
    }
    PoolState state = read_pool(gpu, *system, emitter, kCapacity);
    require_ok(system->release());
    require_ok(gpu.device->check_debug_messages());
    return state;
}

void check_spawn_and_count(Backend backend) {
    Gpu gpu = make_gpu(backend);
    const Shaders shaders = load_shaders(backend);
    auto system = take(ParticleSystem::create(*gpu.device, shaders.vertex, shaders.pixel, shaders.simulate, shaders.args));
    const ParticleEmitterId emitter = take(system->add_emitter(steady_emitter(kCapacity)));
    for (std::uint32_t frame = 0; frame < 8; ++frame) {
        run_frame(gpu, *system, camera_frame(0.25f));
        // Particle n is alive while its age is below one second: frame n spawns, and it ends four frames later.
        const std::uint32_t expected = std::min<std::uint32_t>(frame + 1, 4);
        CHECK(read_uint(read_buffer(gpu, system->live_counter(emitter), ResourceState::ShaderRead, 16), 0) == expected);
    }
    CHECK(take(system->stats(emitter)).spawned == 8);
    require_ok(system->release());
    require_ok(gpu.device->check_debug_messages());
}

// Renders the emitter's billboards into a cleared target with the given blend and depth, and returns the target's rows.
std::vector<std::uint8_t> render_billboards(Gpu& gpu, ParticleSystem& system, TextureHandle color, TextureHandle depth, float depth_clear) {
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(color, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->transition(depth, ResourceState::Common, ResourceState::DepthWrite));
    RenderPassDesc clear;
    clear.color_targets = {color};
    clear.depth_target = depth;
    clear.clear_color = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
    clear.clear_depth = depth_clear;
    require_ok(list->begin_render_pass(clear));
    require_ok(list->end_render_pass());
    require_ok(system.draw(*list, ParticleTargets{color, depth}));
    submit_and_wait(gpu, *list);

    const std::uint64_t pitch = engine::rhi::copy_row_pitch(kImage, Format::R16G16B16A16Float);
    BufferHandle readback = take(gpu.device->create_buffer(buffer_desc(pitch * kImage, BufferUsage::CpuRead | BufferUsage::CopyDestination, "target readback")));
    auto copy = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(copy->transition(color, ResourceState::RenderTarget, ResourceState::CopySource));
    require_ok(copy->copy_texture_to_buffer(color, readback, 0));
    require_ok(copy->transition(color, ResourceState::CopySource, ResourceState::RenderTarget));
    submit_and_wait(gpu, *copy);
    std::vector<std::uint8_t> rows(pitch * kImage);
    require_ok(gpu.device->read_buffer(readback, 0, rows.size(), rows.data()));
    require_ok(gpu.device->destroy(readback));
    // The target is left in RenderTarget for the next frame, and the depth in DepthWrite.
    return rows;
}

void check_billboards(Backend backend) {
    // Each blend and each depth clear is a fresh device, so each case starts from the same state.
    const auto run = [backend](ParticleBlend blend, float depth_clear) {
        Gpu gpu = make_gpu(backend);
        const Shaders shaders = load_shaders(backend);
        auto system = take(ParticleSystem::create(*gpu.device, shaders.vertex, shaders.pixel, shaders.simulate, shaders.args));
        TextureHandle sprite = make_sprite(gpu);
        require_ok(system->set_texture(sprite));
        EmitterDesc desc = steady_emitter(kCapacity);
        desc.blend = blend;
        static_cast<void>(take(system->add_emitter(desc)));
        run_frame(gpu, *system, camera_frame(0.25f));
        run_frame(gpu, *system, camera_frame(0.25f));

        TextureDesc color_desc;
        color_desc.width = kImage;
        color_desc.height = kImage;
        color_desc.format = Format::R16G16B16A16Float;
        color_desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        color_desc.debug_name = "particle target";
        TextureDesc depth_desc = color_desc;
        depth_desc.format = Format::D32Float;
        depth_desc.usage = TextureUsage::DepthStencil;
        depth_desc.debug_name = "particle depth";
        TextureHandle color = take(gpu.device->create_texture(color_desc));
        TextureHandle depth = take(gpu.device->create_texture(depth_desc));
        const std::vector<std::uint8_t> rows = render_billboards(gpu, *system, color, depth, depth_clear);
        require_ok(gpu.device->check_debug_messages());
        require_ok(system->release());
        require_ok(gpu.device->destroy(color));
        require_ok(gpu.device->destroy(depth));
        require_ok(gpu.device->destroy(sprite));
        return rows;
    };

    // The billboards sit at the centre, five metres ahead, so the centre pixel is inside them.
    const std::vector<std::uint8_t> alpha = run(ParticleBlend::Alpha, 1.0f);
    const glm::vec4 alpha_centre = pixel(alpha, kImage / 2, kImage / 2);
    CHECK(alpha_centre.r > 0.5f);
    CHECK(alpha_centre.a > 0.5f);

    // Additive adds the colour and leaves the target's alpha at its clear value.
    const std::vector<std::uint8_t> additive = run(ParticleBlend::Additive, 1.0f);
    const glm::vec4 additive_centre = pixel(additive, kImage / 2, kImage / 2);
    CHECK(additive_centre.r > 0.5f);
    CHECK(additive_centre.a == 0.0f);

    // A depth of zero is nearer than any fragment, so the LESS test rejects every billboard and the target stays clear.
    const std::vector<std::uint8_t> occluded = run(ParticleBlend::Alpha, 0.0f);
    bool any = false;
    for (std::uint32_t y = 0; y < kImage; ++y) {
        for (std::uint32_t x = 0; x < kImage; ++x) {
            const glm::vec4 value = pixel(occluded, x, y);
            any = any || value.r != 0.0f || value.g != 0.0f || value.b != 0.0f || value.a != 0.0f;
        }
    }
    CHECK_FALSE(any);
}

void check_reproducible(Backend backend) {
    const PoolState first = run_seeded(backend, 7);
    const PoolState second = run_seeded(backend, 7);
    CHECK(first.live == second.live);
    CHECK(first.pool == second.pool);

    // The seed matters: another seed gives other particles.
    const PoolState other = run_seeded(backend, 8);
    CHECK(first.pool != other.pool);

    // The particles are real: at least one live slot moves, every live position is finite, and the spawn spread varies.
    std::uint32_t moving = 0;
    for (std::uint32_t slot = 0; slot < kCapacity; ++slot) {
        float record[8];
        std::memcpy(record, first.pool.data() + slot * kParticleStrideBytes, sizeof(record));
        const bool alive = record[3] < record[7];
        if (!alive) {
            continue;
        }
        CHECK(std::isfinite(record[0]));
        CHECK(std::isfinite(record[4]));
        if (std::abs(record[4]) + std::abs(record[5]) + std::abs(record[6]) > 0.0f) {
            ++moving;
        }
    }
    CHECK(moving == first.live);
    CHECK(first.live > 0);
}

void check_budget_pause_reset_and_deletion(Backend backend) {
    Gpu gpu = make_gpu(backend);
    const Shaders shaders = load_shaders(backend);
    ParticleBudget budget;
    budget.max_particles = 64;
    budget.max_emitters = 4;
    auto system = take(ParticleSystem::create(*gpu.device, shaders.vertex, shaders.pixel, shaders.simulate, shaders.args, budget));
    const std::uint32_t after_create = take(gpu.device->resource_stats()).buffers;

    // Overflow: four particles a frame into two slots. The live count holds at the capacity, and the rest are counted as dropped.
    EmitterDesc tight = steady_emitter(2);
    tight.spawn_rate = 16.0f;
    const ParticleEmitterId bounded = take(system->add_emitter(tight));
    for (int frame = 0; frame < 3; ++frame) {
        run_frame(gpu, *system, camera_frame(0.25f));
        CHECK(read_uint(read_buffer(gpu, system->live_counter(bounded), ResourceState::ShaderRead, 16), 0) == 2);
    }
    const engine::render::ParticleEmitterStats tight_stats = take(system->stats(bounded));
    CHECK(tight_stats.spawned == 6);
    CHECK(tight_stats.dropped == 6);
    require_ok(system->remove_emitter(bounded));

    // Pause: a paused emitter's pool and live count are unchanged by later frames.
    const ParticleEmitterId steady = take(system->add_emitter(steady_emitter(kCapacity)));
    CHECK(refused(system->add_emitter(steady_emitter(1)), engine::rhi::RhiErrorCode::OutOfRange));
    for (int frame = 0; frame < 3; ++frame) {
        run_frame(gpu, *system, camera_frame(0.25f));
    }
    const PoolState before_pause = read_pool(gpu, *system, steady, kCapacity);
    require_ok(system->set_paused(steady, true));
    run_frame(gpu, *system, camera_frame(0.25f));
    run_frame(gpu, *system, camera_frame(0.25f));
    const PoolState while_paused = read_pool(gpu, *system, steady, kCapacity);
    CHECK(while_paused.pool == before_pause.pool);
    CHECK(while_paused.live == before_pause.live);
    CHECK(take(system->stats(steady)).paused);

    // Reset: the next frame kills every particle and spawns one, so the live count is one.
    require_ok(system->set_paused(steady, false));
    require_ok(system->reset(steady));
    run_frame(gpu, *system, camera_frame(0.25f));
    CHECK(read_pool(gpu, *system, steady, kCapacity).live == 1);

    // Deletion releases the emitter's five buffers and three sets, and the budget it held.
    require_ok(system->remove_emitter(steady));
    CHECK(take(gpu.device->resource_stats()).buffers == after_create);
    CHECK(system->emitter_count() == 0);
    require_ok(system->release());
    CHECK(take(gpu.device->resource_stats()).buffers == 0);
    require_ok(gpu.device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 GPU particles spawn on the schedule and count their live particles", "[gpu][dx12]") {
    check_spawn_and_count(Backend::Dx12);
}

TEST_CASE("Vulkan GPU particles spawn on the schedule and count their live particles", "[gpu][vulkan]") {
    check_spawn_and_count(Backend::Vulkan);
}

TEST_CASE("DX12 seeded GPU particles reproduce bit for bit within the backend", "[gpu][dx12]") {
    check_reproducible(Backend::Dx12);
}

TEST_CASE("Vulkan seeded GPU particles reproduce bit for bit within the backend", "[gpu][vulkan]") {
    check_reproducible(Backend::Vulkan);
}

TEST_CASE("DX12 billboards draw with alpha and additive blending and against the scene depth", "[gpu][dx12]") {
    check_billboards(Backend::Dx12);
}

TEST_CASE("Vulkan billboards draw with alpha and additive blending and against the scene depth", "[gpu][vulkan]") {
    check_billboards(Backend::Vulkan);
}

TEST_CASE("DX12 GPU particles bound overflow, hold pause and reset, and release on deletion", "[gpu][dx12]") {
    check_budget_pause_reset_and_deletion(Backend::Dx12);
}

TEST_CASE("Vulkan GPU particles bound overflow, hold pause and reset, and release on deletion", "[gpu][vulkan]") {
    check_budget_pause_reset_and_deletion(Backend::Vulkan);
}
