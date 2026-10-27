#include "dx12/dx12_common.hpp"

#include <cstdio>

namespace engine::rhi::dx12 {

void set_name(ID3D12Object* object, const std::string& name) {
    if (name.empty()) {
        return;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (length <= 1) {
        return;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<std::size_t>(length - 1));
    object->SetName(wide.c_str());
}

std::string hresult_text(HRESULT hr) {
    char number[16];
    std::snprintf(number, sizeof number, "0x%08lX", static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    std::string text = number;
    if (hr == DXGI_ERROR_DEVICE_REMOVED) {
        text += " (DXGI_ERROR_DEVICE_REMOVED)";
    } else if (hr == DXGI_ERROR_DEVICE_RESET) {
        text += " (DXGI_ERROR_DEVICE_RESET)";
    } else if (hr == DXGI_ERROR_INVALID_CALL) {
        text += " (DXGI_ERROR_INVALID_CALL)";
    }
    return text;
}

RhiError hresult_error(std::string_view operation, HRESULT hr) {
    const bool removed = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
    return RhiError{removed ? RhiErrorCode::DeviceLost : RhiErrorCode::InvalidState,
                    std::string(operation) + ": a DirectX call failed with " + hresult_text(hr)};
}

std::string wide_to_utf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(length - 1));  // drop the terminating NUL the conversion wrote
    return utf8;
}

DXGI_FORMAT to_dxgi(Format format) noexcept {
    switch (format) {
        case Format::Unknown:
            return DXGI_FORMAT_UNKNOWN;
        case Format::R8G8B8A8Unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8Srgb:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::B8G8R8A8Unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::R16G16B16A16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::R32Uint:
            return DXGI_FORMAT_R32_UINT;
        case Format::D32Float:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::R32G32B32Float:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::R32G32Float:
            return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32G32B32A32Float:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_STATES to_d3d(ResourceState state) noexcept {
    switch (state) {
        case ResourceState::Common:
            return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::VertexBuffer:
        case ResourceState::ConstantBuffer:
            return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        case ResourceState::IndexBuffer:
            return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case ResourceState::ShaderRead:
            // Non-pixel stages only: compute queues accept no pixel-shader state. Pixel reads need their own state with US-032.
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ResourceState::UnorderedAccess:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::RenderTarget:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::DepthWrite:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::DepthRead:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ResourceState::IndirectArgument:
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case ResourceState::CopySource:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDestination:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::Present:
            return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

}  // namespace engine::rhi::dx12
