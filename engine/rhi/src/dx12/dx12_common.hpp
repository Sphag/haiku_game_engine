#pragma once

// Shared Windows and DirectX headers for the DX12 backend. Only first-party code in engine/rhi/src/dx12 includes these; the
// public RHI headers stay free of OS and DirectX types.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::rhi::dx12 {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// "0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)" style text for diagnostics. Unknown codes print only the number.
[[nodiscard]] std::string hresult_text(HRESULT hr);

// Maps a failed DirectX call to an RHI error. Device removal and reset become DeviceLost; everything else is InvalidState.
[[nodiscard]] RhiError hresult_error(std::string_view operation, HRESULT hr);

// Converts a UTF-16 string from the OS (adapter names) to UTF-8.
[[nodiscard]] std::string wide_to_utf8(const wchar_t* text);

[[nodiscard]] DXGI_FORMAT to_dxgi(Format format) noexcept;

// The D3D12 state for a tracked ResourceState. Common maps to D3D12_RESOURCE_STATE_COMMON.
[[nodiscard]] D3D12_RESOURCE_STATES to_d3d(ResourceState state) noexcept;

// Names a D3D12 object for the debug layer and tools. Empty names are skipped.
void set_name(ID3D12Object* object, const std::string& name);

}  // namespace engine::rhi::dx12
