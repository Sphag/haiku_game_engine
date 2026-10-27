#pragma once

// HDR post-process passes on the GPU (US-039). The scene renderer draws linear HDR color and motion vectors; PostProcessor turns them into
// the displayed 8-bit image: TAA resolve with a motion-vector history, a separable bloom blur, exposure and ACES tone mapping, the sRGB encode,
// and the reference overlay composited last. The formulas are the CPU reference in postprocess.hpp, and shaders/postprocess.hlsl implements
// them. Each run is one render graph (render_graph.hpp): the passes, the barriers between them, and a copy of the display image into a
// readback buffer. execute() waits for the submission, so the readback holds the frame when run() returns.
//
// History. The resolved frame is kept in one of two HDR targets, ping-ponged each run. A reset (first frame, size change, camera cut, or
// teleport) makes the next frame keep only its current color (taa_history_reset in postprocess.hpp gives the reason).

#include "engine/render/material.hpp"
#include "engine/render/postprocess.hpp"

#include "engine/rhi/device.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace engine::render {

// Bloom blur taps on each side of the centre (9 taps in all). The shader and the parameter buffer have this many weights.
inline constexpr std::uint32_t kBloomRadius = 4;

struct PostProcessSettings {
    bool enabled = false;          // the scene renderer draws HDR and shows the post output (SceneRendererDesc::post)
    bool taa = true;               // jitter the projection and resolve with history; false shows the current frame only
    TaaSettings taa_settings;      // blend weight of the current frame
    float ev100 = 0.0f;            // manual exposure, in EV100; ExposureSettings::ev_compensation is added to it
    ExposureSettings exposure;
    BloomSettings bloom;
};

// The compiled entries of engine/render/shaders/postprocess.hlsl, for the device's backend. All four are compute stages.
struct PostShaders {
    rhi::ShaderBinary taa;               // CSTaa
    rhi::ShaderBinary bloom_horizontal;  // CSBloomHorizontal
    rhi::ShaderBinary bloom_vertical;    // CSBloomVertical
    rhi::ShaderBinary tonemap;           // CSTonemap
};

// What one run reads. The HDR color and the motion vectors are the scene renderer's targets; the readback is the buffer that receives
// the display image (RGBA8 rows at copy_row_pitch). All three are in the states the scene renderer leaves them in.
struct PostInputs {
    rhi::TextureHandle color;     // R16G16B16A16Float, linear HDR scene color
    rhi::TextureHandle velocity;  // R32G32Float motion vectors in UV units
    rhi::BufferHandle readback;   // CopyDestination state; receives the display image
    glm::dvec3 camera_position{0.0};
    bool camera_cut = false;      // the caller moved the camera to a new shot this frame
};

class PostProcessor {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<PostProcessor>> create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                               const PostProcessSettings& settings, PostShaders shaders);

    // Runs every pass on `inputs` and copies the display image into inputs.readback. Fails when the handles are not from this device or
    // the graph is rejected. Waits for the submission before it returns.
    [[nodiscard]] rhi::RhiStatus run(const PostInputs& inputs);

    // Uploads the overlay that is composited last, in display space with straight alpha. It must be width x height. Until it is set, the
    // overlay is fully transparent.
    [[nodiscard]] rhi::RhiStatus set_overlay(const ImageRgba8& overlay);

    // Changes the target size. The history is discarded, so the next run is a reset (a resize reset).
    [[nodiscard]] rhi::RhiStatus resize(std::uint32_t width, std::uint32_t height);

    // Replaces the exposure, bloom, and TAA settings. The enabled flag is fixed at create and is ignored here.
    void set_settings(const PostProcessSettings& settings) noexcept;

    // Discards the history: the next run keeps only its current frame. Used for a camera cut the caller knows about.
    void reset_history() noexcept {
        has_history_ = false;
    }

    [[nodiscard]] const PostProcessSettings& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    // Why the last run discarded its history (None when it kept it).
    [[nodiscard]] HistoryReset last_reset() const noexcept {
        return last_reset_;
    }

    // Destroys every device object this processor created. The processor is not usable afterward.
    [[nodiscard]] rhi::RhiStatus release();

    PostProcessor(const PostProcessor&) = delete;
    PostProcessor& operator=(const PostProcessor&) = delete;

private:
    // The four compute passes, in the order they run (and the order of `passes_`).
    static constexpr std::size_t kPassTaa = 0;
    static constexpr std::size_t kPassBloomHorizontal = 1;
    static constexpr std::size_t kPassBloomVertical = 2;
    static constexpr std::size_t kPassTonemap = 3;
    static constexpr std::size_t kPassCount = 4;
    // Byte size of the parameter buffer (postprocess.hlsl, kParamWeights and the blur taps).
    static constexpr std::uint64_t kParamBytes = 80;

    PostProcessor(rhi::Device& device, std::uint32_t width, std::uint32_t height, const PostProcessSettings& settings);
    [[nodiscard]] rhi::RhiStatus build(const PostShaders& shaders);
    [[nodiscard]] rhi::RhiStatus build_targets();
    [[nodiscard]] rhi::RhiStatus write_sets();
    [[nodiscard]] rhi::RhiStatus upload_overlay_bytes(const std::vector<std::uint8_t>& bytes);
    void destroy_targets(rhi::RhiStatus& first);

    rhi::Device& device_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    PostProcessSettings settings_;
    // One layout per pass, holding exactly the bindings that pass reads and writes. The RHI checks every binding of a set at dispatch,
    // so a shared layout would need its unused slots in the right state too.
    std::array<rhi::DescriptorSetLayoutHandle, kPassCount> layouts_{};
    std::array<rhi::PipelineHandle, kPassCount> pipelines_{};
    std::array<rhi::DescriptorSetHandle, kPassCount> sets_{};
    std::array<rhi::TextureHandle, 2> history_{};  // the resolved frame, ping-ponged
    std::array<rhi::TextureHandle, 2> bloom_{};    // blur intermediate and blur result
    rhi::TextureHandle display_;
    rhi::TextureHandle overlay_;
    rhi::BufferHandle params_;         // GPU copy of the parameter buffer
    rhi::BufferHandle params_upload_;  // CPU-written staging for params_, filled each run
    std::size_t history_out_ = 0;      // index of the history target this run writes; the other one is read
    bool has_history_ = false;
    std::uint32_t history_width_ = 0;  // the size the history was resolved at
    std::uint32_t history_height_ = 0;
    glm::dvec3 previous_camera_{0.0};
    bool has_camera_ = false;
    HistoryReset last_reset_ = HistoryReset::None;
    bool released_ = false;
};

}  // namespace engine::render
