#pragma once

#include "dx12/dx12_device.hpp"

#include "engine/rhi/swapchain.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace engine::rhi::dx12 {

// Flip-model swapchain on the device's graphics queue. Each back buffer remembers the timeline value of the frame that presented
// it; acquiring that buffer again waits for that value, so frames in flight are bounded by the buffer count.
class Dx12Swapchain final : public Swapchain {
public:
    Dx12Swapchain(Dx12Device& device, ComPtr<IDXGISwapChain3> swap, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                  UINT swap_flags, PresentSettings settings, std::uint32_t buffer_count);
    ~Dx12Swapchain() override;

    RhiResult<std::optional<TextureHandle>> acquire_back_buffer() override;
    RhiStatus present() override;
    RhiStatus resize(std::uint32_t width, std::uint32_t height) override;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] PresentSettings present_settings() const noexcept override;

    // Wraps the back buffers DXGI currently holds. Called on creation and after each resize.
    RhiStatus adopt_buffers();

private:
    void release_buffers();

    Dx12Device& device_;
    ComPtr<IDXGISwapChain3> swap_;
    DXGI_FORMAT format_;
    UINT swap_flags_;
    PresentSettings settings_;  // effective: vsync is forced on when tearing is unavailable
    std::uint32_t width_;       // 0 while the surface is empty
    std::uint32_t height_;
    std::vector<TextureHandle> buffers_;
    std::vector<std::uint64_t> frame_values_;  // graphics timeline value of the frame that last presented each buffer
    std::uint32_t current_ = 0;
    bool acquired_ = false;
};

}  // namespace engine::rhi::dx12
