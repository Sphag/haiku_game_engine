#include "dx12/dx12_swapchain.hpp"
#if ENGINE_FSR2_AVAILABLE
#include "dx12/dx12_upscaler.hpp"
#endif

#include "engine/core/result.hpp"

#include <string>
#include <utility>

namespace engine::rhi::dx12 {
namespace {

constexpr std::uint32_t kMinBufferCount = 2;
constexpr std::uint32_t kMaxBufferCount = 16;

}  // namespace

Dx12Swapchain::Dx12Swapchain(Dx12Device& device, ComPtr<IDXGISwapChain3> swap, std::uint32_t width, std::uint32_t height,
                             DXGI_FORMAT format, UINT swap_flags, PresentSettings settings, std::uint32_t buffer_count)
    : device_(device),
      swap_(std::move(swap)),
      format_(format),
      swap_flags_(swap_flags),
      settings_(settings),
      width_(width),
      height_(height),
      frame_values_(buffer_count, 0) {}

Dx12Swapchain::~Dx12Swapchain() {
    // The back buffers may still be presented or read by queued work. Wait, then release them before the swapchain goes away.
    static_cast<void>(device_.wait_idle());
    release_buffers();
}

std::uint32_t Dx12Swapchain::width() const noexcept {
    return width_;
}

std::uint32_t Dx12Swapchain::height() const noexcept {
    return height_;
}

PresentSettings Dx12Swapchain::present_settings() const noexcept {
    return settings_;
}

RhiStatus Dx12Swapchain::adopt_buffers() {
    buffers_.clear();
    for (std::uint32_t index = 0; index < frame_values_.size(); ++index) {
        ComPtr<ID3D12Resource> resource;
        if (const HRESULT hr = swap_->GetBuffer(index, IID_PPV_ARGS(&resource)); FAILED(hr)) {
            return core::failure(hresult_error("swapchain: GetBuffer", hr));
        }
        TextureDesc desc;
        desc.width = width_;
        desc.height = height_;
        desc.mip_levels = 1;
        desc.format = format_ == DXGI_FORMAT_R8G8B8A8_UNORM ? Format::R8G8B8A8Unorm : Format::B8G8R8A8Unorm;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        desc.debug_name = "swapchain back buffer " + std::to_string(index);
        RhiResult<TextureHandle> handle = device_.adopt_back_buffer(std::move(resource), desc);
        if (!handle.ok()) {
            return core::failure(handle.error());
        }
        buffers_.push_back(handle.value());
    }
    frame_values_.assign(buffers_.size(), 0);
    return core::success();
}

void Dx12Swapchain::release_buffers() {
    for (const TextureHandle& handle : buffers_) {
        static_cast<void>(device_.release_back_buffer(handle));
    }
    buffers_.clear();
}

RhiResult<std::optional<TextureHandle>> Dx12Swapchain::acquire_back_buffer() {
    if (acquired_) {
        return fail<std::optional<TextureHandle>>(RhiErrorCode::InvalidState,
                                                  "acquire_back_buffer: the previous back buffer was not presented");
    }
    if (width_ == 0 || height_ == 0) {
        return core::success(std::optional<TextureHandle>{});
    }
    current_ = swap_->GetCurrentBackBufferIndex();
    if (RhiStatus status = device_.wait_timeline(QueueType::Graphics, frame_values_[current_]); !status.ok()) {
        return core::failure(status.error());
    }
    acquired_ = true;
    return core::success(std::optional<TextureHandle>{buffers_[current_]});
}

RhiStatus Dx12Swapchain::present() {
    if (!acquired_) {
        return fail(RhiErrorCode::InvalidState, "present: no back buffer was acquired for this frame");
    }
    const TextureObject* back = device_.find_texture(buffers_[current_]);
    if (back == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "present: the back buffer handle is no longer live");
    }
    if (back->state != ResourceState::Present) {
        return fail(RhiErrorCode::StateMismatch, "present: the back buffer must be in the Present state; transition it before presenting");
    }
    // Present is queued on the graphics queue after the frame's work, and the timeline signal below follows it.
    const UINT sync_interval = settings_.vsync ? 1 : 0;
    const UINT present_flags = settings_.allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    if (const HRESULT hr = swap_->Present(sync_interval, present_flags); FAILED(hr)) {
        return core::failure(hresult_error("present", hr));
    }
    const RhiResult<std::uint64_t> frame = device_.signal_timeline(QueueType::Graphics);
    if (!frame.ok()) {
        return core::failure(frame.error());
    }
    frame_values_[current_] = frame.value();
    acquired_ = false;
    return core::success();
}

RhiStatus Dx12Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    if (acquired_) {
        return fail(RhiErrorCode::InvalidState, "resize: a back buffer is acquired; present it before resizing");
    }
    if (width == 0 || height == 0) {
        // Minimized: keep the buffers, and acquire returns nothing until a real size arrives.
        width_ = 0;
        height_ = 0;
        return core::success();
    }
    if (width == width_ && height == height_) {
        return core::success();
    }
    if (RhiStatus idle = device_.wait_idle(); !idle.ok()) {
        return idle;
    }
    release_buffers();
    const HRESULT hr = swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swap_flags_);
    if (FAILED(hr)) {
        // The old buffers are still valid in DXGI; restore them so the swapchain stays usable.
        width_ = 0;
        height_ = 0;
        static_cast<void>(adopt_buffers());
        return core::failure(hresult_error("resize: ResizeBuffers", hr));
    }
    width_ = width;
    height_ = height;
    return adopt_buffers();
}

// ---- Dx12Device members that the swapchain uses ----

RhiResult<TextureHandle> Dx12Device::adopt_back_buffer(ComPtr<ID3D12Resource> resource, const TextureDesc& desc) {
    auto object = std::make_shared<TextureObject>();
    object->desc = desc;
    object->resource = std::move(resource);
    object->state = ResourceState::Present;
    object->swapchain_owned = true;
    const auto slot = rtv_pool_.allocate(object->rtv);
    if (!slot) {
        return fail<TextureHandle>(RhiErrorCode::OutOfRange, "swapchain: no render-target descriptors left for a back buffer");
    }
    object->rtv_slot = *slot;
    device_->CreateRenderTargetView(object->resource.Get(), nullptr, object->rtv);
    return core::success(textures_.add(std::move(object), device_id_));
}

RhiStatus Dx12Device::release_back_buffer(TextureHandle handle) {
    std::shared_ptr<TextureObject> object = textures_.take(handle, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "release_back_buffer: handle is invalid or already released");
    }
    release_descriptors(*object);
    retire(std::move(object));
    return core::success();
}

RhiResult<std::unique_ptr<Upscaler>> Dx12Device::create_upscaler(const UpscalerDesc& desc) {
    if (desc.max_render_width == 0 || desc.max_render_height == 0 || desc.display_width == 0 || desc.display_height == 0) {
        return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::OutOfRange, "create_upscaler: every size must be greater than zero");
    }
    if (desc.max_render_width > desc.display_width || desc.max_render_height > desc.display_height) {
        return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::OutOfRange, "create_upscaler: the render size must not exceed the display size");
    }
#if ENGINE_FSR2_AVAILABLE
    auto upscaler = std::make_unique<Dx12Upscaler>(*this, desc);
    if (RhiStatus status = upscaler->initialize(debug_layers_); !status.ok()) {
        return core::failure(status.error());
    }
    return core::success(std::unique_ptr<Upscaler>(std::move(upscaler)));
#else
    return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::Unsupported,
                                           "create_upscaler: FSR 2 is not built; its shader permutation headers are not vendored");
#endif
}

RhiResult<std::unique_ptr<Swapchain>> Dx12Device::create_swapchain(const SwapchainDesc& desc) {
    if (desc.hwnd == nullptr) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor, "create_swapchain: hwnd is null");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor,
                                                "create_swapchain: the surface has no pixels; create it after the window has a size");
    }
    if (desc.buffer_count < kMinBufferCount || desc.buffer_count > kMaxBufferCount) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::OutOfRange, "create_swapchain: buffer_count must be between 2 and 16");
    }
    if (desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor,
                                                "create_swapchain: the format must be B8G8R8A8Unorm or R8G8B8A8Unorm");
    }

    // Tearing needs the adapter's support and the vsync request off. Otherwise vsync stays on, and the settings report it.
    const bool tearing_requested = desc.present.allow_tearing && tearing_supported_;
    PresentSettings effective;
    effective.vsync = desc.present.vsync || !tearing_requested;
    effective.allow_tearing = !effective.vsync;
    const UINT swap_flags = tearing_requested ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = desc.width;
    description.Height = desc.height;
    description.Format = to_dxgi(desc.format);
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = desc.buffer_count;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = swap_flags;

    const HWND window = static_cast<HWND>(desc.hwnd);
    ComPtr<IDXGISwapChain1> swap1;
    if (const HRESULT hr = factory()->CreateSwapChainForHwnd(queue(QueueType::Graphics), window, &description, nullptr, nullptr, &swap1);
        FAILED(hr)) {
        return core::failure(hresult_error("create_swapchain: CreateSwapChainForHwnd", hr));
    }
    // Alt+Enter must not switch to exclusive fullscreen behind the engine's back.
    factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain3> swap3;
    if (FAILED(swap1.As(&swap3))) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::Unsupported, "create_swapchain: IDXGISwapChain3 is required");
    }

    auto swapchain = std::make_unique<Dx12Swapchain>(*this, std::move(swap3), desc.width, desc.height, to_dxgi(desc.format),
                                                      swap_flags, effective, desc.buffer_count);
    if (RhiStatus status = swapchain->adopt_buffers(); !status.ok()) {
        return core::failure(status.error());
    }
    std::unique_ptr<Swapchain> result = std::move(swapchain);
    return core::success(std::move(result));
}

}  // namespace engine::rhi::dx12
