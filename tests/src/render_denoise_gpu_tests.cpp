// The temporal denoiser on real GPUs (US-045). Each case runs TemporalDenoiser on DX12 and on Vulkan with the debug layers on, over a short
// sequence of frames: a moving wall, a disocclusion that changes one column's depth, and a camera cut on the third frame. After each frame
// the output is read back and compared with denoise_reference. The cases also check the API's refusals before the inputs and the frame.
// Any validation error fails the check.
#include "engine/render/denoiser.hpp"
#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::render::DenoiseInputs;
using engine::render::DenoiseReference;
using engine::render::DenoiseSettings;
using engine::render::DenoiseTarget;
using engine::render::TemporalDenoiser;
using engine::render::denoise_reference;
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
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;

namespace {

constexpr std::uint32_t kWidth = 16;
constexpr std::uint32_t kHeight = 8;
constexpr int kFrames = 4;
// The history stores half floats, so a value near 2 carries about 2e-3 of rounding per frame. The tolerance covers a few frames of it.
constexpr float kTolerance = 0.02f;

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

ShaderBinary denoise_shader(Backend backend) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/denoise_temporal" + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return ShaderBinary{ShaderStage::Compute, "CSTemporalDenoise", std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>())};
}

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

// The device, a fence, and the staging buffers, which stay alive until the case ends so the GPU never reads freed upload memory.
struct Gpu {
    std::unique_ptr<Device> device;
    FenceHandle fence;
    std::uint64_t value = 0;
    std::vector<BufferHandle> staging;
};

Gpu make_gpu(Backend backend) {
    Gpu gpu;
    gpu.device = take(engine::rhi::create_device(debug_device(backend)));
    gpu.fence = take(gpu.device->create_fence(0));
    return gpu;
}

// One texel per (x, y), packed for the format: R16G16B16A16Float (8 bytes), R32Float (4 bytes), R32G32Float (8 bytes), in rows of the copy
// pitch.
std::vector<std::uint8_t> half_rows(const std::vector<glm::vec4>& texels, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(width, Format::R16G16B16A16Float);
    std::vector<std::uint8_t> bytes(pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint64_t texel = glm::packHalf4x16(texels[static_cast<std::size_t>(y) * width + x]);
            std::memcpy(bytes.data() + y * pitch + x * 8, &texel, sizeof(texel));
        }
    }
    return bytes;
}

std::vector<std::uint8_t> float_rows(const std::vector<float>& texels, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(width, Format::R32Float);
    std::vector<std::uint8_t> bytes(pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(bytes.data() + y * pitch, texels.data() + static_cast<std::size_t>(y) * width, width * sizeof(float));
    }
    return bytes;
}

std::vector<std::uint8_t> float2_rows(const std::vector<glm::vec2>& texels, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(width, Format::R32G32Float);
    std::vector<std::uint8_t> bytes(pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(bytes.data() + y * pitch, texels.data() + static_cast<std::size_t>(y) * width, width * sizeof(glm::vec2));
    }
    return bytes;
}

TextureHandle make_texture(Gpu& gpu, Format format, TextureUsage usage, const char* name) {
    TextureDesc desc;
    desc.width = kWidth;
    desc.height = kHeight;
    desc.format = format;
    desc.usage = usage;
    desc.debug_name = name;
    return take(gpu.device->create_texture(desc));
}

// Copies `rows` into `texture`, which is in `before`, and leaves it in ShaderRead. The staging buffer is kept until the case ends.
void upload_texture(Gpu& gpu, CommandList& list, TextureHandle texture, const std::vector<std::uint8_t>& rows, ResourceState before) {
    const std::uint64_t size = rows.size();
    const BufferHandle staging = take(gpu.device->create_buffer(buffer_desc(size, BufferUsage::CopySource | BufferUsage::CpuWrite, "denoise staging")));
    require_ok(gpu.device->upload_buffer(staging, 0, size, rows.data()));
    gpu.staging.push_back(staging);
    require_ok(list.transition(texture, before, ResourceState::CopyDestination));
    require_ok(list.copy_buffer_to_texture(staging, 0, texture));
    require_ok(list.transition(texture, ResourceState::CopyDestination, ResourceState::ShaderRead));
}

// Reads a texture in ShaderRead back to the host, in rows of the copy pitch. The texture returns to ShaderRead.
std::vector<std::uint8_t> read_texture(Gpu& gpu, TextureHandle texture, std::uint64_t size) {
    const BufferHandle readback = take(gpu.device->create_buffer(buffer_desc(size, BufferUsage::CopyDestination | BufferUsage::CpuRead, "denoise readback")));
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(texture, ResourceState::ShaderRead, ResourceState::CopySource));
    require_ok(list->copy_texture_to_buffer(texture, readback, 0));
    require_ok(list->transition(texture, ResourceState::CopySource, ResourceState::ShaderRead));
    require_ok(list->close());
    std::vector<engine::rhi::CommandList*> lists{list.get()};
    require_ok(gpu.device->submit(QueueType::Graphics, lists, gpu.fence, ++gpu.value));
    require_ok(gpu.device->wait_fence(gpu.fence, gpu.value));
    std::vector<std::uint8_t> bytes(size);
    require_ok(gpu.device->read_buffer(readback, 0, size, bytes.data()));
    require_ok(gpu.device->destroy(readback));
    return bytes;
}

// The synthetic sequence: a wall whose pattern moves one pixel to the right each frame, a column that steps 2 m closer on frame 1, and
// the camera cut on frame 2. Every value is a multiple of a quarter, so the half-float upload is exact.
DenoiseTarget sequence_frame(int frame) {
    DenoiseTarget target;
    target.width = kWidth;
    target.height = kHeight;
    const std::size_t count = static_cast<std::size_t>(kWidth) * kHeight;
    target.value.resize(count);
    target.depth.resize(count);
    target.normal.resize(count);
    target.motion.resize(count);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::size_t i = target.index(x, y);
            target.value[i] = glm::vec4(static_cast<float>((x + 3 * y + frame) % 9) * 0.25f, static_cast<float>(x % 4) * 0.25f, 0.5f, 1.0f);
            target.depth[i] = (frame == 1 && x == 7) ? 8.0f : 10.0f;
            target.normal[i] = x == 5 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
            // One pixel to the right in UV units (1 / 16 is exact).
            target.motion[i] = frame == 0 ? glm::vec2(0.0f) : glm::vec2(1.0f / static_cast<float>(kWidth), 0.0f);
        }
    }
    return target;
}

void check_frame_against_reference(const std::vector<std::uint8_t>& gpu_bytes, const DenoiseReference& reference, int frame) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(kWidth, Format::R16G16B16A16Float);
    std::size_t mismatches = 0;
    std::size_t non_finite = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            std::uint64_t texel = 0;
            std::memcpy(&texel, gpu_bytes.data() + y * pitch + x * 8, sizeof(texel));
            const glm::vec4 gpu = glm::unpackHalf4x16(texel);
            const glm::vec4 expected = reference.output[reference.index(x, y)];
            if (!std::isfinite(gpu.r) || !std::isfinite(gpu.g) || !std::isfinite(gpu.b) || !std::isfinite(gpu.a)) {
                ++non_finite;
            }
            if (glm::any(glm::greaterThan(glm::abs(gpu - expected), glm::vec4(kTolerance)))) {
                ++mismatches;
            }
        }
    }
    INFO("frame " << frame);
    CHECK(non_finite == 0);
    CHECK(mismatches == 0);
}

void check_sequence(Backend backend) {
    Gpu gpu = make_gpu(backend);
    DenoiseSettings settings;
    settings.spatial_radius = 1;
    settings.history_weight = 0.9f;
    settings.max_age = 15;

    auto pass = take(TemporalDenoiser::create(*gpu.device, kWidth, kHeight, denoise_shader(backend)));
    const TextureHandle value = make_texture(gpu, Format::R16G16B16A16Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "denoise input value");
    const TextureHandle depth = make_texture(gpu, Format::R32Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "denoise input depth");
    const TextureHandle normal = make_texture(gpu, Format::R16G16B16A16Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "denoise input normal");
    const TextureHandle motion = make_texture(gpu, Format::R32G32Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "denoise input motion");
    require_ok(pass->set_inputs(DenoiseInputs{value, depth, normal, motion}));

    engine::render::DenoiseHistory history;
    bool any_accepted = false;
    for (int frame = 0; frame < kFrames; ++frame) {
        const DenoiseTarget target = sequence_frame(frame);
        // The cut is on frame 2. The reference restarts there, and the GPU must do the same.
        const bool reset = frame == 2;
        const DenoiseReference reference = denoise_reference(target, history, settings, reset);
        for (const std::uint8_t accepted : reference.accepted) {
            any_accepted = any_accepted || accepted != 0;
        }
        if (reset) {
            for (const std::uint8_t accepted : reference.accepted) {
                CHECK(accepted == 0);
            }
        }

        // The frame's settings are uploaded first (through the graph), then the inputs and the dispatch in one list.
        require_ok(pass->update_frame(settings, reset));
        auto list = take(gpu.device->create_command_list(QueueType::Graphics));
        const ResourceState before = frame == 0 ? ResourceState::Common : ResourceState::ShaderRead;
        upload_texture(gpu, *list, value, half_rows(target.value, kWidth, kHeight), before);
        upload_texture(gpu, *list, depth, float_rows(target.depth, kWidth, kHeight), before);
        std::vector<glm::vec4> normals;
        for (const glm::vec3& v : target.normal) {
            normals.emplace_back(v, 0.0f);
        }
        upload_texture(gpu, *list, normal, half_rows(normals, kWidth, kHeight), before);
        upload_texture(gpu, *list, motion, float2_rows(target.motion, kWidth, kHeight), before);
        require_ok(pass->record(*list));
        require_ok(list->close());
        std::vector<engine::rhi::CommandList*> lists{list.get()};
        require_ok(gpu.device->submit(QueueType::Graphics, lists, gpu.fence, ++gpu.value));
        require_ok(gpu.device->wait_fence(gpu.fence, gpu.value));

        const std::uint64_t size = engine::rhi::copy_row_pitch(kWidth, Format::R16G16B16A16Float) * kHeight;
        check_frame_against_reference(read_texture(gpu, pass->output(), size), reference, frame);
        history = reference.history;
    }
    CHECK(any_accepted);
    require_ok(gpu.device->check_debug_messages());
    require_ok(pass->release());
}

void check_api_order(Backend backend) {
    Gpu gpu = make_gpu(backend);
    auto pass = take(TemporalDenoiser::create(*gpu.device, kWidth, kHeight, denoise_shader(backend)));
    auto list = take(gpu.device->create_command_list(QueueType::Graphics));
    CHECK_FALSE(pass->record(*list).ok());
    CHECK_FALSE(pass->output().valid());
    const TextureHandle texture = make_texture(gpu, Format::R32Float, TextureUsage::ShaderResource, "denoise probe");
    CHECK_FALSE(pass->set_inputs(DenoiseInputs{}).ok());
    require_ok(pass->set_inputs(DenoiseInputs{make_texture(gpu, Format::R16G16B16A16Float, TextureUsage::ShaderResource, "value"), texture,
                                              make_texture(gpu, Format::R16G16B16A16Float, TextureUsage::ShaderResource, "normal"),
                                              make_texture(gpu, Format::R32G32Float, TextureUsage::ShaderResource, "motion")}));
    CHECK_FALSE(pass->record(*list).ok());
    DenoiseSettings bad;
    bad.max_age = 0;
    CHECK_FALSE(pass->update_frame(bad, false).ok());
    require_ok(list->close());
}

}  // namespace

TEST_CASE("DX12 temporal denoiser matches its CPU reference over a moving sequence with a disocclusion and a camera cut", "[gpu][dx12]") {
    check_sequence(Backend::Dx12);
}

TEST_CASE("Vulkan temporal denoiser matches its CPU reference over a moving sequence with a disocclusion and a camera cut", "[gpu][vulkan]") {
    check_sequence(Backend::Vulkan);
}

TEST_CASE("DX12 temporal denoiser refuses a record before its inputs and its frame", "[gpu][dx12]") {
    check_api_order(Backend::Dx12);
}

TEST_CASE("Vulkan temporal denoiser refuses a record before its inputs and its frame", "[gpu][vulkan]") {
    check_api_order(Backend::Vulkan);
}
