#pragma once

// Options shared by the GPU suites. ENGINE_GPU_ALLOW_SOFTWARE=1 lets a software adapter run them (WARP for DX12, a CPU device for
// Vulkan). That is a development aid for checking code paths on a machine without a GPU: the adapter is printed as software, and a
// run with it is not a GPU report. CI never sets it.

#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#include <cstring>
#endif

namespace engine::tests {

inline bool allow_software_adapters() {
#ifdef _WIN32
    // _dupenv_s instead of getenv: MSVC's getenv is a deprecation warning, and the first-party build treats warnings as errors.
    char* value = nullptr;
    std::size_t length = 0;
    const bool enabled = _dupenv_s(&value, &length, "ENGINE_GPU_ALLOW_SOFTWARE") == 0 && value != nullptr && std::string_view(value) == "1";
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv("ENGINE_GPU_ALLOW_SOFTWARE");
    return value != nullptr && std::string_view(value) == "1";
#endif
}

}  // namespace engine::tests
